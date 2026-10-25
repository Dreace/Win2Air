#include "presentation_standby_scenarios.h"

namespace Win2Air::Tests {

void ReceiverReportedLatencyScenario(std::filesystem::path const& root) {
  auto reported_root = root / L"receiver-reported-latency";
  auto configuration = Save(reported_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "adaptive";
  configuration.settings.silence_timeout = 0;
  SettingsStore(reported_root).Save(configuration);
  VirtualReceiver peer;
  peer.receiver_render_ms = 69;
  auto source = std::make_shared<VirtualAudioState>();
  StreamCoordinator service(peer, reported_root, false,
                            VirtualAudioServices(source));
  auto identity = configuration.receivers.front().receiver_id;
  Require(
      Complete(service.Connect(identity)).status == CommandStatus::applied &&
          service.Snapshot().timing.base_ms == 100 &&
          service.Snapshot().timing.receiver_render_ms == 69,
      "Receiver-reported latency is rounded before capture starts");
  Require(Complete(service.Disconnect()).status == CommandStatus::applied,
          "End the first receiver-reported session");
  {
    std::lock_guard lock(peer.mutex);
    peer.receiver_render_ms = 76;
  }
  Require(
      Complete(service.Connect(identity)).status == CommandStatus::applied &&
          service.Snapshot().timing.base_ms == 110,
      "Reconnection reads the receiver again instead of reusing a target");
  Complete(service.Disconnect());
  {
    std::lock_guard lock(peer.mutex);
    peer.receiver_render_ms.reset();
  }
  auto missing = Complete(service.Connect(identity));
  Require(
      missing.status == CommandStatus::failed &&
          missing.message.find("arrivalToRenderLatencyMs") != std::string::npos,
      "A missing receiver report stops adaptive startup with an explicit "
      "reason");
  service.Shutdown(true).get();
}
void AdaptiveLossRecoveryScenario(std::filesystem::path const& root) {
  auto adaptation_root = root / L"adaptive-loss-recovery";
  auto configuration = Save(adaptation_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "adaptive";
  configuration.settings.silence_timeout = 0;
  SettingsStore(adaptation_root).Save(configuration);
  VirtualReceiver peer;
  peer.receiver_render_ms = 65;
  auto source = std::make_shared<VirtualAudioState>();
  std::atomic<uint64_t> metrics_now{1000000000};
  auto hooks = VirtualAudioServices(source);
  hooks.metrics_now = [&] { return metrics_now.load(); };
  std::atomic<unsigned> adaptive_notices{};
  hooks.log_sink = [&](LogRecord const& record) {
    if (record.event == LogEvent::adaptive_latency &&
        record.level >= LogLevel::info)
      ++adaptive_notices;
  };
  StreamCoordinator service(peer, adaptation_root, false, hooks);
  auto identity = configuration.receivers.front().receiver_id;
  Require(
      Complete(service.Connect(identity)).status == CommandStatus::applied &&
          service.Snapshot().timing.base_ms == 90,
      "Adaptive recovery starts at the receiver-derived target");
  source->Push(NonzeroPacket(441));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 441; },
            "Voiced audio establishes the adaptive RTP timeline");
  auto step = [&](unsigned second, uint64_t resends, int expected) {
    {
      std::lock_guard lock(peer.mutex);
      peer.metrics.sent_packets += 125;
      peer.metrics.resend_requests += resends;
    }
    metrics_now = uint64_t(second) * 1000000000ULL;
    WaitUntil(
        [&] {
          auto snapshot = service.Snapshot();
          return snapshot.metrics.sample_time == metrics_now &&
                 snapshot.timing.base_ms == expected;
        },
        "Adaptive timing did not follow the observed retransmission window");
  };
  step(2, 0, 90);
  step(3, 1, 90);
  step(4, 1, 90);
  step(5, 1, 100);
  WaitUntil([&] { return adaptive_notices.load() >= 2; },
            "Adaptive instability and target changes were not retained at "
            "Info or above");
  step(6, 1, 100);
  step(7, 1, 100);
  step(8, 1, 110);
  for (unsigned second = 9; second < 38; ++second) step(second, 0, 110);
  step(38, 0, 100);
  source->Push(NonzeroPacket(441));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 882; },
            "Audio resumes after automatic recovery re-anchors the timeline");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.created == 1 && peer.presentation_requests == 1 &&
                peer.accepted.back().timing.sample_index == 0 &&
                peer.accepted.back().timing.timeline_epoch >
                    peer.accepted.front().timing.timeline_epoch,
            "Automatic recovery avoids overlapping scheduled audio without "
            "reconnecting");
  }
  step(39, 1, 100);
  step(40, 1, 100);
  step(41, 1, 110);
  step(42, 3, 120);
  for (unsigned second = 43; second <= 90; ++second) step(second, 0, 120);
  Require(Complete(service.Disconnect()).status == CommandStatus::applied &&
              Complete(service.Connect(identity)).status ==
                  CommandStatus::applied &&
              service.Snapshot().timing.base_ms == 90,
          "Reconnection clears the elevated connection target");
  step(91, 0, 90);
  step(92, 1, 90);
  step(93, 1, 90);
  step(94, 1, 100);
  for (unsigned second = 95; second < 124; ++second) step(second, 0, 100);
  step(124, 0, 90);
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "A reconnected session can recover automatically again");
}
void LiveSliderLatencyScenario(std::filesystem::path const& root) {
  auto live_root = root / L"live-slider-latency";
  auto configuration = Save(live_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "custom";
  configuration.settings.custom_ms = 160;
  configuration.settings.silence_timeout = 0;
  SettingsStore(live_root).Save(configuration);
  VirtualReceiver peer;
  auto source = std::make_shared<VirtualAudioState>();
  StreamCoordinator service(peer, live_root, false,
                            VirtualAudioServices(source));
  auto identity = configuration.receivers.front().receiver_id;
  Require(Complete(service.Connect(identity)).status == CommandStatus::applied,
          "Connect once before changing a voiced stream");
  source->Push(NonzeroPacket(441));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 441; },
            "Submit voiced audio before reducing the budget");
  Require(Complete(service.SetLatency("custom", 80)).status ==
                  CommandStatus::applied &&
              service.Snapshot().timing.base_ms == 80,
          "Reducing custom latency disconnects and reconnects once");
  Require(Complete(service.SetOffset(identity, -10)).status ==
                  CommandStatus::applied &&
              service.Snapshot().timing.effective_ms == 70,
          "Negative device compensation applies without reconnecting");
  source->Push(NonzeroPacket(441));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 882; },
            "Fresh audio resumes on the new presentation anchor");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.created == 2 && peer.presentation_requests == 1 &&
                peer.accepted.back().timing.sample_index == 0 &&
                peer.accepted.back().timing.timeline_epoch >
                    peer.accepted.front().timing.timeline_epoch,
            "A custom delay change opens a new session and device "
            "compensation stays on it");
  }
  service.Shutdown(true).get();
}
void SavedAuthenticationScenario(std::filesystem::path const& root) {
  auto password_root = root / L"saved-authentication";
  auto configuration = Save(password_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "ultra";
  SettingsStore(password_root).Save(configuration);
  auto id = configuration.receivers.front().receiver_id;
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  peer.required_password = "receiver-password";
  peer.export_credentials = true;
  StreamCoordinator service(peer, password_root, false,
                            VirtualAudioServices(source));
  Require(
      Complete(service.Connect(id, SecureBuffer("receiver-password"))).status ==
          CommandStatus::applied,
      "The receiver accepts the user-provided password");
  auto secret_path = password_root / L"authentication/AA-BB-CC-DD-EE-FF.bin";
  auto encrypted = StorageIO::Read(secret_path);
  Require(std::string(encrypted.begin(), encrypted.end())
                  .find("receiver-password") == std::string::npos,
          "Saved authentication input is encrypted at rest");
  Require(Complete(service.SetLatency("balanced", 1000)).status ==
              CommandStatus::applied,
          "Changing latency reuses the saved receiver password");
  service.Shutdown(true).get();
  StreamCoordinator restored(peer, password_root, false,
                             VirtualAudioServices(source));
  Require(Complete(restored.Connect(id)).status == CommandStatus::applied,
          "Restarting the application reuses its encrypted password");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.password_connections == 3,
            "Initial, latency-change and restart connections authenticate");
    peer.required_password = "rotated-password";
  }
  Require(Complete(restored.SetLatency("stable", 2000)).status ==
                  CommandStatus::failed &&
              !std::filesystem::exists(secret_path),
          "An explicitly rejected saved password is removed");
  Require(!restored.Snapshot().reconnect,
          "Rejected authentication waits for user input instead of retrying");
  Require(
      Complete(restored.Connect(id, SecureBuffer("rotated-password"))).status ==
          CommandStatus::applied,
      "Entering the replacement password restores the connection");
  Require(
      Complete(restored.ForgetPairing(id)).status == CommandStatus::applied &&
          !std::filesystem::exists(secret_path),
      "Forgetting the receiver also removes its saved authentication input");
  restored.Shutdown(true).get();
}
}  // namespace Win2Air::Tests
