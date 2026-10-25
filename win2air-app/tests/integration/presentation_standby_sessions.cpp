#include "presentation_standby_scenarios.h"

namespace Win2Air::Tests {

void StandbyDisconnectScenario(std::filesystem::path const& root) {
  auto idle_root = root / L"standby-disconnect";
  auto config = Save(idle_root);
  config.settings.silence_timeout = 10;
  config.settings.local_playback = false;
  ++config.settings.revision;
  SettingsStore(idle_root).Save(config);
  auto source = std::make_shared<VirtualAudioState>();
  auto services = VirtualAudioServices(source);
  std::atomic<uint64_t> now{1000000000};
  services.silence_now = services.metrics_now = [&] { return now.load(); };
  VirtualReceiver peer;
  StreamCoordinator service(peer, idle_root, false, services);
  auto identity = config.receivers.front().receiver_id;
  Require(Complete(service.Connect(identity)).status == CommandStatus::applied,
          "Connect the idle-disconnect virtual receiver");
  auto advance = [&](uint64_t seconds) {
    now += seconds * 1000000000;
    WaitUntil([&] { return service.Snapshot().metrics.sample_time >= now; },
              "Idle lifecycle clock was not sampled");
  };
  auto waiting = [&] {
    auto state = service.Snapshot().session->session;
    return state && state->state == SessionState::disconnected &&
           !state->generation;
  };
  advance(11);
  Require(service.Snapshot().session->session->standby_state ==
              StandbyState::standby,
          "Silence enters standby before the disconnect timer starts");
  advance(600);
  Require(!waiting(), "Idle disconnect is disabled by default");
  Require(
      Complete(service.SetStandbyDisconnect(60)).status ==
              CommandStatus::applied &&
          SettingsStore(idle_root).Load().settings.standby_disconnect_seconds ==
              60,
      "Persist the idle disconnect preference");
  Require(Complete(service.SetStandbyDisconnect(61)).status ==
                  CommandStatus::failed &&
              service.Snapshot().preferences.standby_disconnect_seconds == 60,
          "Reject unsupported idle disconnect durations atomically");
  Complete(service.SetAutoReconnect(false));
  advance(1);
  advance(58);
  Require(!waiting(),
          "The idle timeout starts after standby, not after connecting");
  advance(1);
  WaitUntil(waiting, "Standby did not close the receiver connection");
  {
    auto notices = service.Snapshot().notices;
    Require(std::any_of(notices.begin(), notices.end(),
                        [](auto const& item) {
                          return item.code == "standby-enter";
                        }) &&
                notices.back().code == "standby-disconnect" &&
                notices.back().receiver_id == identity &&
                notices.back().occurred_at.time_since_epoch().count() > 0,
            "Standby and automatic disconnect retain their device and time");
  }
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.destroyed == 1,
            "Idle disconnect destroys the receiver runtime");
  }
  {
    std::lock_guard lock(source->mutex);
    Require(source->stopped == 0 && source->levels.at("virtual-default").muted,
            "Automatic disconnect retains the capture listener and local mute "
            "lease");
  }
  Require(std::wstring_view(ConnectedStatusKey(SessionState::disconnected,
                                               StandbyState::standby)) ==
              L"StatusWaitingForAudio",
          "Disconnected standby shows waiting for audio");
  service.NotifySystemEvent(SystemEvent::suspend);
  WaitUntil(
      [&] {
        std::lock_guard lock(source->mutex);
        return source->stopped > 0 &&
               !source->levels.at("virtual-default").muted;
      },
      "Suspending the idle listener releases capture and local mute");
  service.NotifySystemEvent(SystemEvent::resume);
  WaitUntil(
      [&] {
        std::lock_guard lock(source->mutex);
        return source->levels.at("virtual-default").muted;
      },
      "Resume restarts audio monitoring");
  Require(waiting(), "System resume keeps the receiver disconnected");
  unsigned released{};
  {
    std::lock_guard lock(source->mutex);
    released = source->released;
  }
  auto quiet = NonzeroPacket(4410);
  std::fill(quiet.bytes.begin(), quiet.bytes.end(), uint8_t{});
  source->Push(std::move(quiet));
  WaitUntil(
      [&] {
        std::lock_guard lock(source->mutex);
        return source->released > released;
      },
      "The disconnected listener did not consume silence");
  Require(waiting(), "Silence does not reconnect the receiver");
  auto previous_generation = service.Snapshot().session->session->generation;
  source->Push(NonzeroPacket(4410));
  WaitUntil(
      [&] {
        auto state = service.Snapshot().session->session;
        return state && state->generation > previous_generation &&
               (state->state == SessionState::ready ||
                state->state == SessionState::streaming);
      },
      "Sound did not reconnect with network auto-retry disabled");
  source->Push(NonzeroPacket(4410));
  WaitUntil([&] { return service.Snapshot().submitted_frames > 0; },
            "Fresh audio did not reach the reconnected receiver");
  {
    auto notices = service.Snapshot().notices;
    Require(std::count_if(notices.begin(), notices.end(),
                          [](auto const& item) {
                            return item.code == "standby-leave";
                          }) == 1,
            "Sound-triggered reconnection records leaving standby once");
  }
  advance(11);
  advance(61);
  WaitUntil(waiting, "The next standby did not disconnect");
  Complete(service.SetStandbyDisconnect(0));
  WaitUntil(
      [&] {
        auto state = service.Snapshot().session->session;
        return state && state->state == SessionState::ready;
      },
      "Disabling idle disconnect did not restore the requested connection");
  {
    auto notices = service.Snapshot().notices;
    Require(std::count_if(notices.begin(), notices.end(),
                          [](auto const& item) {
                            return item.code == "standby-leave";
                          }) == 2,
            "Disabling idle disconnect records leaving standby once");
  }
  Complete(service.SetStandbyDisconnect(60));
  advance(11);
  advance(61);
  WaitUntil(waiting, "Standby did not disconnect before manual cancellation");
  Complete(service.Disconnect());
  source->Push(NonzeroPacket(4410));
  advance(1);
  Require(!service.Snapshot().session->session,
          "Manual disconnect cancels sound-triggered reconnect");
  {
    std::lock_guard lock(source->mutex);
    Require(!source->levels.at("virtual-default").muted && source->stopped > 0,
            "Manual disconnect stops monitoring and restores local playback");
  }
  service.Shutdown(true).get();
}
void ReceiverMediaControlScenario(std::filesystem::path const& root) {
  auto control_root = root / L"receiver-media-control";
  auto config = Save(control_root);
  auto source = std::make_shared<VirtualAudioState>();
  auto clocks = VirtualAudioServices(source);
  std::atomic<uint64_t> clock{1000000000};
  clocks.metrics_now = [&] { return clock.load(); };
  std::atomic<std::shared_ptr<NowPlaying const>> media;
  clocks.now_playing = [&] { return media.load(); };
  std::mutex received_mutex;
  std::vector<uint32_t> received;
  std::vector<LogRecord> control_logs;
  clocks.log_sink = [&](LogRecord const& record) {
    std::lock_guard lock(received_mutex);
    control_logs.push_back(record);
  };
  clocks.media_control = [&](uint32_t command) {
    std::lock_guard lock(received_mutex);
    received.push_back(command);
    return true;
  };
  VirtualReceiver peer;
  StreamCoordinator service(peer, control_root, false, clocks);
  auto identity = config.receivers.front().receiver_id;
  Require(Complete(service.Connect(identity)).status == CommandStatus::applied,
          "Connect a virtual media-control receiver");
  auto send = [&](uint64_t now) {
    {
      std::lock_guard lock(peer.mutex);
      peer.media_commands = {1, 2, 3, 4};
    }
    clock = now;
    WaitUntil(
        [&] {
          std::lock_guard lock(peer.mutex);
          return peer.media_commands.empty();
        },
        "Receiver commands were not consumed");
  };
  send(2000000000);
  WaitUntil(
      [&] {
        std::lock_guard lock(received_mutex);
        return received.size() == 4;
      },
      "Receiver commands did not reach the Windows media boundary");
  {
    auto notices = service.Snapshot().notices;
    for (auto code :
         {"control-pause", "control-play", "control-next", "control-previous"})
      Require(std::any_of(notices.begin(), notices.end(),
                          [&](auto const& item) {
                            return item.code == code &&
                                   item.receiver_id == identity;
                          }),
              "Received media controls appear in connection events");
  }
  auto track = std::make_shared<NowPlaying>();
  track->title = "Virtual song";
  track->artist = "Virtual artist";
  track->revision = 1;
  media = track;
  clock += 1000000000;
  WaitUntil(
      [&] {
        auto notices = service.Snapshot().notices;
        return std::any_of(
            notices.begin(), notices.end(), [](auto const& item) {
              return item.code == "content" &&
                     item.detail == "Virtual song | Virtual artist";
            });
      },
      "Now-playing changes appear in connection events");
  auto artwork = std::make_shared<NowPlaying>(*track);
  artwork->revision = 2;
  artwork->artwork = {1, 2, 3};
  media = artwork;
  clock += 1000000000;
  WaitUntil([&] { return service.Snapshot().metrics.sample_time >= clock; },
            "Updated artwork was not sampled");
  {
    auto notices = service.Snapshot().notices;
    Require(std::count_if(
                notices.begin(), notices.end(),
                [](auto const& item) { return item.code == "content"; }) == 1,
            "Artwork refresh retains a single content-change event");
  }
  Require(Complete(service.SetReceiverMediaControl(false)).status ==
              CommandStatus::applied,
          "Disable receiver media controls");
  send(5000000000);
  {
    std::lock_guard lock(received_mutex);
    Require(received == std::vector<uint32_t>{1, 2, 3, 4},
            "Disabled commands are discarded without changing playback");
  }
  Require(!SettingsStore(control_root).Load().settings.receiver_media_control,
          "Receiver control preference is persisted");
  Complete(service.SetReceiverMediaControl(true));
  send(6000000000);
  WaitUntil(
      [&] {
        std::lock_guard lock(received_mutex);
        return received.size() == 8;
      },
      "Re-enabled controls did not resume");
  Complete(service.Disconnect());
  service.Shutdown(true).get();
  {
    std::lock_guard lock(received_mutex);
    bool queued{}, disabled{}, disconnected{};
    for (auto const& record : control_logs) {
      queued |= record.event == LogEvent::media_control &&
                record.level == LogLevel::info &&
                record.detail.find("outcome=queued") != std::string::npos;
      disabled |= record.event == LogEvent::media_control &&
                  record.detail.find("outcome=disabled") != std::string::npos;
      disconnected |=
          record.event == LogEvent::session_changed &&
          record.detail.find("state=disconnected") != std::string::npos;
    }
    Require(queued && disabled && disconnected,
            "Media actions, ignored commands and disconnects are recorded at "
            "their source");
  }
}
}  // namespace Win2Air::Tests
