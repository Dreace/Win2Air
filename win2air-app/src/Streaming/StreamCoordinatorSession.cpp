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
void StreamCoordinator::Create(PresentationConfig const &config) {
  if (runtime_)
    throw std::runtime_error("Previous sender runtime has not been released");
  sender_error error{};
  auto capacity = SenderAudioCapacity(config);
  sender_create_options options{SENDER_ABI_VERSION,
                                sizeof(options),
                                {sender_id_, "Win2Air", "AirPlay/999.0.0",
                                 capacity, State, Credentials, this},
                                {config.base_ms, config.offset_ms},
                                Step,
                                VolumeCompleted};
  options.on_log = SafeLog;
  options.buffers = {8, kRenderLeadMs, kSendSafetyMs, 1000};
  if (config.mode == "ultra" || config.mode == "low" ||
      config.mode == "adaptive")
    options.buffers.startup_ms = 0;
  Checked(library_.Create(options, &runtime_, &error), error);
}
void StreamCoordinator::Save() {
  auto candidate = settings_;
  candidate.settings.revision++;
  candidate.receivers = registry_.Records();
  try {
    store_.Save(candidate);
  } catch (...) {
    Note(LogLevel::error, LogModule::settings, LogEvent::persistence_failed,
         "operation=save-settings revision=" +
             std::to_string(candidate.settings.revision));
    throw;
  }
  settings_ = std::move(candidate);
  Note(LogLevel::debug, LogModule::settings, LogEvent::settings_changed,
       "revision=" + std::to_string(settings_.settings.revision));
}
void StreamCoordinator::Cleanup(bool restore_local, bool retain_session) {
  idle_disconnected_ = false;
  standby_since_.reset();
  if (restore_local && !retain_session) {
    desired_connected_ = false;
    reconnect_.Reset();
    reconnecting_ = false;
  }
  if (session_.session) {
    session_.session->state = SessionState::ending;
    Publish();
  }
  capture_->Stop();
  capturing_ = false;
  ClearAudio();
  returned_credentials_.reset();
  pending_authentication_input_.Clear();
  authentication_input_used_ = false;
  std::string local_error;
  try {
    if (restore_local) local_->Restore();
    std::lock_guard lock(mutex_);
    snapshot_.local_error.clear();
  } catch (...) {
    local_error = DescribeFailure().message;
  }
  if (connecting_) {
    connecting_result_.status = CommandStatus::canceled;
    connecting_result_.message = "Connection intent canceled";
    connecting_->set_value(connecting_result_);
    connecting_.reset();
  }
  CloseReceiver();
  if (!retain_session) session_.session.reset();
  {
    std::lock_guard lock(mutex_);
    snapshot_.applied_volume.reset();
    snapshot_.volume_pending = false;
    snapshot_.presentation_pending = false;
    snapshot_.timing = {};
  }
  if (!local_error.empty()) {
    std::lock_guard lock(mutex_);
    snapshot_.local_error = local_error;
    throw std::runtime_error(local_error);
  }
}
void StreamCoordinator::CloseReceiver() {
  if (runtime_) {
    sender_error error{};
    if (generation_) {
      auto state = library_.State(runtime_, generation_);
      if (state != SENDER_ENDED && state != SENDER_FAILED) {
        auto result = library_.End(runtime_, generation_, &error);
        if (result != SENDER_OK && result != SENDER_ALREADY_ENDED)
          Checked(result, error);
      }
      auto deadline =
          std::min(std::chrono::steady_clock::now() + std::chrono::seconds(4),
                   shutdown_deadline_.load());
      while (state != SENDER_ENDED && state != SENDER_FAILED &&
             std::chrono::steady_clock::now() < deadline) {
        Checked(library_.Poll(runtime_, 5, &error), error);
        state = library_.State(runtime_, generation_);
      }
      if (state != SENDER_ENDED && state != SENDER_FAILED)
        throw std::runtime_error(
            "Sender did not reach its terminal state before cleanup deadline");
    }
    Checked(library_.Destroy(runtime_, &error), error);
    runtime_ = nullptr;
    generation_ = 0;
    adaptive_generation_ = 0;
    media_generation_ = media_revision_ = volume_observed_revision_ = 0;
    metadata_transport_error_.clear();
  }
}
void StreamCoordinator::RecordConnection(bool established) {
  if (selected_.empty()) return;
  auto record = registry_.Find(selected_);
  if (record.connect_established == established) return;
  record.connect_established = established;
  registry_.Update(record);
  Save();
}
void StreamCoordinator::BeginConnection(
    CommandResult result, SecureBuffer const &pin,
    std::shared_ptr<std::promise<CommandResult>> promise) {
  RecordConnection(false);
  auto record = registry_.Find(selected_);
  if (!record.features)
    throw SenderFailure("Receiver capabilities are unavailable", 0, 0,
                        SENDER_CAPABILITY);
  auto endpoint = SelectedEndpoint(record);
  if (!endpoint)
    throw SenderFailure("Selected receiver has no available endpoint", 0, 0,
                        SENDER_NETWORK);
  auto config = InitialPresentation(settings_.settings, record);
  Create(config);
  callback_failed_ = false;
  active_intent_ = result.intent_epoch;
  sender_target target{endpoint->resolved_address.c_str(),
                       static_cast<uint16_t>(endpoint->port),
                       record.receiver_id.c_str(),
                       record.name.c_str(),
                       *record.features,
                       endpoint->scope_id,
                       EffectiveVolume(record)};
  sender_audio_format format{44100, 16, 2};
  sender_error error{};
  SecureBuffer material;
  std::optional<sender_credentials> credentials;
  auto file = record.receiver_id;
  std::replace(file.begin(), file.end(), ':', '-');
  if (pin.Bytes().empty() && record.identity_verified &&
      std::filesystem::exists(root_ / L"credentials" / (file + ".bin"))) {
    try {
      auto saved = credentials_.Load(selected_, sender_id_);
      material = Terminated(saved);
    } catch (...) {
      throw SenderFailure(
          "Stored credentials require pairing: " + DescribeFailure().message, 0,
          0, SENDER_AUTHENTICATION);
    }
    credentials = sender_credentials{
        selected_.c_str(), sender_id_,
        reinterpret_cast<char const *>(material.Bytes().data())};
  }
  SecureBuffer pairing;
  pending_authentication_input_.Clear();
  if (!pin.Bytes().empty()) {
    pending_authentication_input_ = SecureBuffer(pin.Bytes());
    pairing = Terminated(pin);
  } else if (std::filesystem::exists(root_ / L"authentication" /
                                     (file + ".bin"))) {
    auto saved = authentication_inputs_.Load(selected_, sender_id_);
    pairing = Terminated(saved);
  }
  authentication_input_used_ = !pairing.Bytes().empty();
  Checked(library_.Connect(
              runtime_, &target, &format, credentials ? &*credentials : nullptr,
              pairing.Bytes().empty()
                  ? nullptr
                  : reinterpret_cast<char const *>(pairing.Bytes().data()),
              &generation_, &error),
          error);
  connecting_ = std::move(promise);
  {
    std::lock_guard lock(mutex_);
    metrics_.BeginGeneration(
        generation_,
        audio_services_.metrics_now ? audio_services_.metrics_now()
                                    : MonotonicNs(),
        {snapshot_.captured_frames, snapshot_.submitted_frames,
         snapshot_.nonzero_frames, snapshot_.discontinuities, recoveries_});
  }
  if (!connecting_) reconnecting_ = true;
  connecting_result_ = result;
  connecting_result_.generation = generation_;
  connection_deadline_ =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  session_.last_command = connecting_result_;
  session_.session = ReceiverSession{selected_,
                                     generation_,
                                     active_intent_,
                                     SessionState::connecting,
                                     StandbyState::active,
                                     {},
                                     {},
                                     config,
                                     {}};
  Publish();
}
std::future<CommandResult> StreamCoordinator::Connect(std::string identity,
                                                      SecureBuffer pin) {
  auto promise = std::make_shared<std::promise<CommandResult>>();
  auto future = promise->get_future();
  if (shutting_down_ || stopping_) {
    CommandResult canceled;
    canceled.status = CommandStatus::canceled;
    canceled.message = "Application shutdown in progress";
    promise->set_value(canceled);
    return future;
  }
  auto secret = std::make_shared<SecureBuffer>(std::move(pin));
  ++intent_epoch_;
  std::lock_guard lock(mutex_);
  CommandResult result;
  result.command_id = ++next_command_;
  result.intent_epoch = intent_epoch_;
  commands_.push_back([this, promise, result, identity = std::move(identity),
                       secret]() mutable {
    if (result.intent_epoch != intent_epoch_) {
      auto canceled = result;
      canceled.status = CommandStatus::canceled;
      promise->set_value(canceled);
      return;
    }
    try {
      reconnect_.Reset();
      reconnecting_ = false;
      desired_connected_ = false;
      auto record = registry_.Find(identity);
      selected_ = record.receiver_id;
      settings_.settings.selected_receiver_id = selected_;
      Cleanup();
      suppressed_ = false;
      Save();
      desired_connected_ = true;
      active_intent_ = result.intent_epoch;
      recovery_standby_ = StandbyState::active;
      BeginConnection(result, *secret, promise);
      secret->Clear();
    } catch (...) {
      secret->Clear();
      auto failure = DescribeFailure();
      auto failed = result;
      failed.status = CommandStatus::failed;
      failed.stage = SessionState::error;
      failed.message = failure.message;
      failed.platform_code = failure.platform;
      failed.peer_code = failure.peer;
      failed.sender_code = failure.sender_code;
      Recover(failure);
      try {
        promise->set_value(failed);
      } catch (std::future_error const &) {
      }
    }
  });
  wake_.notify_one();
  return future;
}
std::future<CommandResult> StreamCoordinator::Disconnect() {
  return Queue(true, [this](auto result) {
    reconnect_.Reset();
    reconnecting_ = false;
    desired_connected_ = false;
    suppressed_ = true;
    Cleanup();
    result.status = CommandStatus::applied;
    result.stage = SessionState::disconnected;
    return result;
  });
}
}  // namespace Win2Air
