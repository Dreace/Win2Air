#include "presentation_standby_scenarios.h"

namespace Win2Air::Tests {

void UltraFixedAdaptiveScenario(std::filesystem::path const& root,
                                std::string const& id) {
  auto fixed_root = root / L"ultra-fixed";
  auto fixed = Save(fixed_root);
  ++fixed.settings.revision;
  fixed.settings.latency_mode = "low";
  fixed.settings.silence_timeout = 0;
  SettingsStore(fixed_root).Save(fixed);
  auto fixed_source = std::make_shared<VirtualAudioState>();
  VirtualReceiver fixed_peer;
  std::atomic<uint64_t> fixed_now{1000000000};
  auto fixed_hooks = VirtualAudioServices(fixed_source);
  fixed_hooks.metrics_now = [&] { return fixed_now.load(); };
  StreamCoordinator fixed_service(fixed_peer, fixed_root, false, fixed_hooks);
  Require(Complete(fixed_service.Connect(id)).status == CommandStatus::applied,
          "Low mode starts at its fixed 120 ms budget");
  fixed_source->Push(NonzeroPacket(441));
  WaitUntil([&] { return fixed_service.Snapshot().submitted_frames > 0; },
            "Ultra mode submits current audio before its budget is checked");
  for (unsigned second = 2; second <= 12; ++second) {
    {
      std::lock_guard lock(fixed_peer.mutex);
      fixed_peer.metrics.sent_packets += 125;
    }
    fixed_now = uint64_t(second) * 1000000000;
    WaitUntil(
        [&] {
          return fixed_service.Snapshot().metrics.sample_time == fixed_now;
        },
        "Ultra mode records samples without searching");
  }
  Require(fixed_service.Snapshot().timing.base_ms == 120,
          "Low mode keeps the 120 ms budget");
  {
    std::lock_guard lock(fixed_peer.mutex);
    Require(fixed_peer.presentation_requests == 0 &&
                fixed_peer.retarget_requests == 0,
            "Ultra mode does not move the sender timeline");
  }
  Require(fixed_service.Shutdown(true).get().status == CommandStatus::applied,
          "Fixed ultra streaming ends cleanly");
  auto saved_root = root / L"ultra-ignores-adaptive";
  auto recorded_ultra = Save(saved_root);
  ++recorded_ultra.settings.revision;
  recorded_ultra.settings.latency_mode = "low";
  recorded_ultra.settings.silence_timeout = 0;
  auto& recorded = recorded_ultra.receivers.front();
  recorded.firmware_version = "virtual-firmware";
  recorded.manual_endpoint->interface_index = 1;
  recorded.latency_profile =
      LatencyProfile{"virtual-firmware", "127.0.0.1:7000|if=1|scope=0", 20};
  SettingsStore(saved_root).Save(recorded_ultra);
  auto loaded = SettingsStore(saved_root).Load();
  Require(SavedLatency(loaded.receivers.front(),
                       *loaded.receivers.front().manual_endpoint) == 20,
          "The recorded adaptive budget is available for this receiver");
  VirtualReceiver saved_peer;
  StreamCoordinator saved_service(saved_peer, saved_root, false,
                                  VirtualAudioServices(fixed_source));
  Require(
      Complete(saved_service.Connect(id)).status == CommandStatus::applied &&
          saved_service.Snapshot().timing.base_ms == 120,
      "Low mode keeps 120 ms when an adaptive budget is recorded");
  {
    std::lock_guard lock(saved_peer.mutex);
    Require(saved_peer.retarget_requests == 0 &&
                saved_peer.presentation_requests == 0,
            "Ultra mode does not apply the recorded adaptive budget");
  }
  Require(saved_service.Shutdown(true).get().status == CommandStatus::applied,
          "Ultra mode with a recorded adaptive budget ends cleanly");
}
void AdaptiveRetainedProfileScenario(std::filesystem::path const& root,
                                     std::string const& id) {
  auto ultra_root = root / L"adaptive";
  auto configuration = Save(ultra_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "adaptive";
  configuration.settings.silence_timeout = 0;
  configuration.receivers.front().identity_verified = true;
  configuration.receivers.front().firmware_version = "virtual-firmware";
  configuration.receivers.front().manual_endpoint->interface_index = 1;
  configuration.receivers.front().latency_profile =
      LatencyProfile{"virtual-firmware", "127.0.0.1:7000|if=1|scope=0", 20};
  SettingsStore(ultra_root).Save(configuration);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  std::atomic<uint64_t> observed_now{1000000000};
  auto hooks = VirtualAudioServices(source);
  hooks.metrics_now = [&] { return observed_now.load(); };
  StreamCoordinator service(peer, ultra_root, false, hooks);
  Require(
      Complete(service.Connect(id)).status == CommandStatus::applied,
      "Adaptive mode starts from the receiver report with its safety margin");
  source->Push(NonzeroPacket(441));
  WaitUntil([&] { return service.Snapshot().submitted_frames > 0; },
            "Ultra mode transfers current PCM through the SPSC capture queue");
  for (unsigned second = 2; second <= 370; ++second) {
    {
      std::lock_guard lock(peer.mutex);
      peer.metrics.sent_packets += 125;
    }
    observed_now = uint64_t(second) * 1000000000;
    WaitUntil(
        [&] { return service.Snapshot().metrics.sample_time == observed_now; },
        "The adaptive service consumes the injected monotonic sample");
  }
  auto recorded = SettingsStore(ultra_root).Load().receivers.front();
  Require(service.Snapshot().timing.base_ms == 150 &&
              recorded.latency_profile &&
              recorded.latency_profile->budget_ms == 20,
          "Transport counters keep the startup budget and historical data");
  unsigned search_presentations;
  unsigned search_retargets;
  unsigned search_created;
  {
    std::lock_guard lock(peer.mutex);
    search_presentations = peer.presentation_requests;
    search_retargets = peer.retarget_requests;
    search_created = peer.created;
    Require(search_presentations == 0 && search_retargets == 1,
            "Stable transport keeps the receiver-derived starting target");
  }
  Require(Complete(service.SetLatency("balanced", 1000)).status ==
                  CommandStatus::applied &&
              service.Snapshot().timing.base_ms == 1000 &&
              service.Snapshot().preferences.latency_mode == "balanced",
          "Switching to balanced disconnects and reconnects once");
  Require(Complete(service.SetLatency("adaptive", 120)).status ==
                  CommandStatus::applied &&
              service.Snapshot().timing.base_ms == 150 &&
              service.Snapshot().preferences.latency_mode == "adaptive",
          "Switching back to adaptive uses its normal startup budget");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.presentation_requests == search_presentations &&
                peer.created == search_created + 2 &&
                peer.retarget_requests == search_retargets + 1,
            "Each latency mode opens its own session");
  }
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "Adaptive virtual streaming ends cleanly");
  StreamCoordinator restored_profile(peer, ultra_root, false,
                                     VirtualAudioServices(source));
  Require(
      Complete(restored_profile.Connect(id)).status == CommandStatus::applied &&
          restored_profile.Snapshot().timing.base_ms == 150,
      "Reconnecting ignores the unverified historical target");
  restored_profile.Shutdown(true).get();
}
void AdaptiveStandbyAnchorScenario(std::filesystem::path const& root,
                                   std::string const& id) {
  auto anchor_root = root / L"adaptive-standby-anchor";
  auto configuration = Save(anchor_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "adaptive";
  configuration.settings.silence_timeout = 10;
  SettingsStore(anchor_root).Save(configuration);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  std::atomic<uint64_t> metrics_now{1000000000};
  std::atomic<uint64_t> silence_now{1000000000};
  auto hooks = VirtualAudioServices(source);
  hooks.metrics_now = [&] { return metrics_now.load(); };
  hooks.silence_now = [&] { return silence_now.load(); };
  StreamCoordinator service(peer, anchor_root, false, hooks);
  Require(
      Complete(service.Connect(id)).status == CommandStatus::applied &&
          service.Snapshot().timing.base_ms == 150,
      "Adaptive standby starts from the receiver budget with safety margin");
  for (unsigned second = 2; second <= 3; ++second) {
    {
      std::lock_guard lock(peer.mutex);
      peer.metrics.sent_packets += 125;
    }
    metrics_now = uint64_t(second) * 1000000000;
    WaitUntil(
        [&] {
          return service.Snapshot().metrics.sample_time == metrics_now.load();
        },
        "Adaptive standby records a stable sample");
  }
  Require(service.Snapshot().timing.base_ms == 150,
          "Adaptive standby holds its budget before the source parks");
  silence_now = 12000000000;
  WaitUntil(
      [&] {
        auto state = service.Snapshot().session->session;
        return state && state->standby_state == StandbyState::standby;
      },
      "Silence parks the adaptive source");
  {
    std::lock_guard lock(peer.mutex);
    peer.metrics.sent_packets += 125;
  }
  metrics_now = 4000000000;
  WaitUntil(
      [&] {
        return service.Snapshot().metrics.sample_time == metrics_now.load();
      },
      "The parked adaptive source records the next sample");
  Require(service.Snapshot().timing.base_ms == 150,
          "Parking the source keeps the adaptive budget");
  size_t parked_audio;
  {
    std::lock_guard lock(peer.mutex);
    parked_audio = peer.accepted.size();
  }
  source->Push(NonzeroPacket(4410));
  WaitUntil(
      [&] {
        std::lock_guard lock(peer.mutex);
        return peer.accepted.size() > parked_audio;
      },
      "Voiced audio resumes the parked adaptive source");
  {
    std::lock_guard lock(peer.mutex);
    peer.metrics.sent_packets += 125;
  }
  metrics_now = 5000000000;
  WaitUntil(
      [&] {
        return service.Snapshot().metrics.sample_time == metrics_now.load();
      },
      "The resumed adaptive source records the next sample");
  auto reasons = service.Snapshot().discontinuity_reasons;
  auto parked = reasons.find("silence-standby");
  auto resumed = reasons.find("standby-resume");
  Require(parked != reasons.end() && parked->second == 1 &&
              resumed != reasons.end() && resumed->second == 1 &&
              service.Snapshot().timing.base_ms == 150,
          "Standby park and resume keep the adaptive budget");
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "Adaptive standby ends cleanly");
}
void AdaptiveRejectionRecoveryScenario(std::filesystem::path const& root,
                                       std::string const& id) {
  auto rejected_root = root / L"adaptive-rejection-recovery";
  auto configuration = Save(rejected_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "ultra";
  configuration.settings.silence_timeout = 0;
  SettingsStore(rejected_root).Save(configuration);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  auto hooks = VirtualAudioServices(source);
  StreamCoordinator service(peer, rejected_root, false, hooks);
  Require(Complete(service.Connect(id)).status == CommandStatus::applied,
          "Prepare a receiver that rejects FLUSH while audio is playing");
  source->Push(NonzeroPacket(441));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 441; },
            "Initial current audio must reach the virtual receiver");
  {
    std::lock_guard lock(peer.mutex);
    peer.reject_presentation = peer.interrupt_rejected_presentation = true;
  }
  Require(Complete(service.SetLatency("custom", 100)).status ==
                  CommandStatus::applied &&
              service.Snapshot().timing.base_ms == 100 &&
              service.Snapshot().session->session->state == SessionState::ready,
          "Switching to custom reconnects when the old session would reject "
          "FLUSH");
  source->Push(NonzeroPacket(441));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 882; },
            "Playback continues on the reconnected session");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.created == 2 && peer.retarget_requests == 0 &&
                peer.presentation_requests == 0,
            "The mode switch opens one session");
  }
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "The retargeted session shuts down cleanly");
}
}  // namespace Win2Air::Tests
