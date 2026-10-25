#include "StreamCoordinatorInternal.h"

#include <avrt.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <thread>
#include <utility>

#include "../Settings/StorageIO.h"
#include "FailureDetail.h"
#include "LatencyProfile.h"
#include "ReceiverSession.h"
namespace Win2Air {
StreamCoordinator::StreamCoordinator(SenderBackend &library,
                                     std::filesystem::path root, bool discover,
                                     AudioServices audio_services,
                                     std::unique_ptr<DnsSdBrowser> browser)
    : library_(library),
      store_(root),
      credentials_(root / L"credentials"),
      authentication_inputs_(root / L"authentication"),
      root_(std::move(root)),
      registry_(),
      discovery_enabled_(discover) {
  const bool injected = audio_services.devices || audio_services.capture ||
                        audio_services.local_playback;
  if (injected && (!audio_services.devices || !audio_services.capture ||
                   !audio_services.local_playback))
    throw std::runtime_error(
        "Injected audio services must include devices, capture and local "
        "playback");
  if (injected && discover && !browser)
    throw std::runtime_error(
        "Injected audio services require network discovery to be disabled");
  browser_ = browser ? std::move(browser) : std::make_unique<DnsSdBrowser>();
  audio_services_ = std::move(audio_services);
  std::promise<void> started;
  auto ready = started.get_future();
  worker_ = std::thread([this, promise = std::move(started)]() mutable {
    Run(std::move(promise));
  });
  try {
    ready.get();
  } catch (...) {
    stopping_ = true;
    wake_.notify_all();
    worker_.join();
    throw;
  }
}
StreamCoordinator::~StreamCoordinator() {
  stopping_ = true;
  ++intent_epoch_;
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (runtime_) library_.RetainUntilProcessExit();
}
PlaybackSnapshot StreamCoordinator::Snapshot() {
  std::lock_guard lock(mutex_);
  return snapshot_;
}
void StreamCoordinator::Publish() {
  auto id = session_.session ? session_.session->receiver_id : std::string{};
  auto generation = session_.session ? session_.session->generation : 0;
  auto state =
      session_.session ? session_.session->state : SessionState::disconnected;
  auto standby =
      session_.session ? session_.session->standby_state : StandbyState::active;
  if (!id.empty() && id == logged_session_id_ &&
      (state == SessionState::ready || state == SessionState::streaming)) {
    if (standby == StandbyState::standby && logged_standby_ != standby)
      AddNotice("standby-enter");
    else if (logged_standby_ == StandbyState::standby &&
             standby != StandbyState::standby)
      AddNotice("standby-leave");
  }
  if (id != logged_session_id_ || generation != logged_session_generation_ ||
      state != logged_session_state_ || standby != logged_standby_) {
    constexpr char const *states[]{"disconnected", "connecting", "pairing",
                                   "setting-up",   "ready",      "streaming",
                                   "recovering",   "ending",     "error"};
    constexpr char const *phases[]{"active", "silence-pending", "standby",
                                   "resuming"};
    // Quiet audio is transient; retain actual standby transitions at Info.
    auto level = id == logged_session_id_ &&
                         generation == logged_session_generation_ &&
                         state == logged_session_state_ &&
                         (standby == StandbyState::silence_pending ||
                          (logged_standby_ == StandbyState::silence_pending &&
                           standby == StandbyState::active))
                     ? LogLevel::debug
                     : LogLevel::info;
    Note(level, LogModule::streaming, LogEvent::session_changed,
         std::string("state=") + states[unsigned(state)] +
             " standby=" + phases[unsigned(standby)]);
    logged_session_id_ = id;
    logged_session_generation_ = generation;
    logged_session_state_ = state;
    logged_standby_ = standby;
  }
  session_.revision = ++revision_;
  session_.intent.selected_receiver_id = selected_;
  session_.intent.intent_epoch = intent_epoch_;
  session_.intent.desired_connected = desired_connected_;
  session_.intent.suppressed_this_run = suppressed_;
  std::lock_guard lock(mutex_);
  snapshot_.session = std::make_shared<SessionSnapshot const>(session_);
  snapshot_.receivers = registry_.Records();
  snapshot_.local_playback = settings_.settings.local_playback;
  snapshot_.source_config = settings_.settings.audio_source;
  snapshot_.preferences = settings_.settings;
  snapshot_.discovery_refreshing = discovery_refresh_.valid();
  snapshot_.reconnect = reconnect_.Snapshot();
  snapshot_.recovery_now = RecoveryNow();
}
std::future<CommandResult> StreamCoordinator::Queue(
    bool supersede, std::function<CommandResult(CommandResult)> action,
    bool shutdown) {
  auto promise = std::make_shared<std::promise<CommandResult>>();
  auto future = promise->get_future();
  if ((shutting_down_ && !shutdown) || stopping_) {
    CommandResult canceled;
    canceled.status = CommandStatus::canceled;
    canceled.message = "Application shutdown in progress";
    promise->set_value(canceled);
    return future;
  }
  if (supersede) ++intent_epoch_;
  std::lock_guard lock(mutex_);
  CommandResult result;
  result.command_id = ++next_command_;
  result.intent_epoch = intent_epoch_;
  commands_.push_back([this, promise, result, action = std::move(action)] {
    if (result.intent_epoch != intent_epoch_) {
      auto canceled = result;
      canceled.status = CommandStatus::canceled;
      promise->set_value(canceled);
      return;
    }
    try {
      Note(LogLevel::debug, LogModule::app, LogEvent::command_submitted,
           "command_id=" + std::to_string(result.command_id));
      auto applied = action(result);
      auto level = applied.status == CommandStatus::failed ? LogLevel::error
                   : applied.status == CommandStatus::rejected
                       ? LogLevel::warning
                       : LogLevel::debug;
      auto event = applied.status == CommandStatus::failed
                       ? LogEvent::command_failed
                   : applied.status == CommandStatus::rejected
                       ? LogEvent::command_rejected
                   : applied.status == CommandStatus::canceled
                       ? LogEvent::command_canceled
                       : LogEvent::command_applied;
      auto detail = "command_id=" + std::to_string(result.command_id);
      if (!applied.message.empty()) detail += " detail=" + applied.message;
      Note(level, LogModule::app, event, std::move(detail), applied.sender_code,
           applied.platform_code, applied.peer_code);
      session_.last_command = applied;
      Publish();
      promise->set_value(applied);
    } catch (...) {
      auto detail = DescribeFailure();
      auto failed = result;
      Note(LogLevel::error, LogModule::app, LogEvent::command_failed,
           "command_id=" + std::to_string(result.command_id) +
               " error=" + detail.message,
           detail.sender_code, detail.platform, detail.peer);
      failed.status = CommandStatus::failed;
      failed.stage = SessionState::error;
      failed.message = detail.message;
      failed.platform_code = detail.platform;
      failed.peer_code = detail.peer;
      failed.sender_code = detail.sender_code;
      session_.last_command = failed;
      if (session_.session && session_.session->state == SessionState::ending)
        Fail(detail.message, detail.platform, detail.peer);
      else
        Publish();
      promise->set_value(failed);
    }
  });
  wake_.notify_one();
  return future;
}
void StreamCoordinator::Run(std::promise<void> started) {
  bool opened = false, apartment = false;
  std::unique_ptr<void, decltype(&AvRevertMmThreadCharacteristics)> scheduler(
      nullptr, AvRevertMmThreadCharacteristics);
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    apartment = true;
    if (!audio_services_.capture) {
      DWORD task{};
      scheduler.reset(AvSetMmThreadCharacteristicsW(L"Audio", &task));
      if (!scheduler) winrt::throw_last_error();
      if (!AvSetMmThreadPriority(scheduler.get(), AVRT_PRIORITY_HIGH))
        winrt::throw_last_error();
    }
    StorageIO::PrivateDirectory(root_);
    local_ =
        audio_services_.local_playback
            ? audio_services_.local_playback(root_ / L"recovery")
            : std::make_unique<LocalPlaybackController>(root_ / L"recovery");
    if (!local_)
      throw std::runtime_error(
          "Local playback factory returned an empty service");
    local_->Recover();
    settings_ = store_.Load();
    if (audio_services_.log_level_changed)
      audio_services_.log_level_changed(
          ResolveLogLevel(settings_.settings.log_level));
    Note(LogLevel::info, LogModule::app, LogEvent::application_lifecycle,
         "operation=streaming-started log_level=" +
             std::string(
                 LogLevelName(ResolveLogLevel(settings_.settings.log_level))));
    if (!audio_services_.capture && !audio_services_.now_playing)
      system_media_ =
          std::make_unique<SystemMediaSession>(audio_services_.log_sink);
    selected_ = settings_.settings.selected_receiver_id;
    registry_ = ReceiverRegistry(settings_.receivers);
    sender_id_ = SenderIdentity(root_);
    devices_ = audio_services_.devices ? audio_services_.devices()
                                       : std::make_unique<DeviceManager>();
    capture_ = audio_services_.capture ? audio_services_.capture()
                                       : std::make_unique<WasapiCapture>();
    if (!local_ || !devices_ || !capture_)
      throw std::runtime_error(
          "Audio services factory returned an empty service");
    capture_->OnReady([this] {
      {
        std::lock_guard lock(mutex_);
        capture_ready_ = true;
      }
      wake_.notify_one();
    });
    converter_ = std::make_unique<FormatConverter>(
        audio_services_.transform,
        audio_services_.audio_now ? audio_services_.audio_now : MonotonicNs);
    {
      std::lock_guard lock(mutex_);
      snapshot_.sources = devices_->Enumerate();
      snapshot_.discovering = discovery_enabled_;
    }
    if (discovery_enabled_) browser_->Start(discovery_epoch_);
    Publish();
    started.set_value();
    opened = true;
    while (!stopping_) {
      std::function<void()> command;
      {
        std::unique_lock lock(mutex_);
        if (commands_.empty() && !capture_ready_)
          wake_.wait_for(lock, std::chrono::milliseconds(5), [this] {
            return !commands_.empty() || capture_ready_ || stopping_;
          });
        capture_ready_ = false;
        if (!commands_.empty()) {
          command = std::move(commands_.front());
          commands_.pop_front();
        }
      }
      if (command) command();
      if (stopping_) break;
      try {
        SystemEvents();
        TickRecovery();
        auto events = devices_->TakeEvents();
        if (!events.empty()) {
          auto devices = devices_->Enumerate();
          {
            std::lock_guard lock(mutex_);
            snapshot_.sources = devices;
          }
          if (capturing_) {
            auto next = ResolveSource();
            auto match = std::find_if(
                devices.begin(), devices.end(),
                [&](auto const &value) { return value.id == next; });
            if (next != endpoint_ || match == devices.end() ||
                match->mix_format != mix_) {
              capture_->Stop();
              capturing_ = false;
              local_->Restore();
              FlushSource();
              StartSource();
            }
          }
          Publish();
        }
        Pump();
        SampleMetrics();
        DrainLogs();
        if (!runtime_ && desired_connected_) Publish();
      } catch (...) {
        Recover(DescribeFailure());
      }
      try {
        Discover();
      } catch (...) {
        auto detail = DescribeFailure();
        bool changed;
        {
          std::lock_guard lock(mutex_);
          changed =
              snapshot_.discovery_error != detail.message ||
              snapshot_.discovery_refreshing != discovery_refresh_.valid();
          snapshot_.discovery_error = detail.message;
          snapshot_.discovery_refreshing = discovery_refresh_.valid();
        }
        if (changed) {
          Note(LogLevel::warning, LogModule::discovery,
               LogEvent::discovery_changed,
               "operation=discovery-failed error=" + detail.message,
               detail.sender_code, detail.platform, detail.peer);
          Publish();
        }
      }
    }
    if (!exit_cleanup_failed_) Cleanup();
    StopDiscovery();
  } catch (...) {
    if (!opened)
      started.set_exception(std::current_exception());
    else {
      auto detail = DescribeFailure();
      Fail(detail.message, detail.platform, detail.peer);
    }
  }
  try {
    StopDiscovery();
  } catch (...) {
    if (opened) Fail(DescribeFailure().message);
  }
  converter_.reset();
  Note(LogLevel::info, LogModule::app, LogEvent::application_lifecycle,
       "operation=streaming-stopped");
  capture_.reset();
  devices_.reset();
  local_.reset();
  if (apartment) winrt::uninit_apartment();
}
}  // namespace Win2Air
