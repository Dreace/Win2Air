#include "presentation_standby_scenarios.h"

namespace Win2Air::Tests {

void MinimalLeadScenario(std::filesystem::path const& root,
                         std::string const& id) {
  auto minimal_root = root / L"minimal-lead";
  auto configuration = Save(minimal_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "custom";
  configuration.settings.custom_ms = 20;
  SettingsStore(minimal_root).Save(configuration);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  auto hooks = VirtualAudioServices(source);
  std::atomic<uint64_t> audio_now{1000000000};
  hooks.audio_now = [&] { return audio_now.load(); };
  StreamCoordinator service(peer, minimal_root, false, hooks);
  Require(Complete(service.Connect(id)).status == CommandStatus::applied,
          "A persisted minimal sender budget connects through the real "
          "coordinator and virtual receiver");
  uint64_t submitted{};
  uint64_t position{};
  unsigned reconnects = 1;
  for (int budget : {120, 100, 80, 60, 40, 30, 20}) {
    ++reconnects;
    Require(Complete(service.SetLatency("custom", budget)).status ==
                    CommandStatus::applied &&
                service.Snapshot().timing.effective_ms == budget &&
                peer.created == reconnects,
            "A custom delay change disconnects and reconnects once");
    auto packet = NonzeroPacket(441, audio_now.load() / 100);
    packet.position = position;
    position += packet.frames;
    source->Push(std::move(packet));
    submitted += 441;
    audio_now += 441ull * 1000000000ull / 44100ull;
    WaitUntil([&] { return service.Snapshot().submitted_frames == submitted; },
              "Current PCM must continue after each presentation change");
  }
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.created == 8 && peer.accepted.size() == 7 &&
                peer.timing_with_pending_audio == 0,
            "Each custom delay change reconnects and keeps the submitted "
            "audio");
  }
  Require(service.Snapshot().timing.capacity_bytes <= 16 * 44100 / 1000 * 4 &&
              SettingsStore(minimal_root).Load().settings.custom_ms == 20,
          "Minimal lead bounds the SDK queue and survives settings reload");
  auto revision = service.Snapshot().preferences.revision;
  Require(Complete(service.SetLatency("custom", 19)).status ==
                  CommandStatus::failed &&
              service.Snapshot().preferences.revision == revision,
          "An invalid budget preserves the applied timeline and settings");
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "Minimal lead streaming shuts down cleanly");
}
void ContinuousDiscontinuityScenario(std::filesystem::path const& root,
                                     std::string const& id) {
  auto gap_root = root / L"continuous-discontinuity";
  auto configuration = Save(gap_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "ultra";
  configuration.settings.silence_timeout = 0;
  SettingsStore(gap_root).Save(configuration);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  std::atomic<uint64_t> audio_now{50000000000};
  auto hooks = VirtualAudioServices(source);
  hooks.audio_now = [&] { return audio_now.load(); };
  StreamCoordinator service(peer, gap_root, false, hooks);
  Require(Complete(service.Connect(id)).status == CommandStatus::applied,
          "Flagged capture connects before audio is submitted");
  unsigned baseline_flushes;
  {
    std::lock_guard lock(peer.mutex);
    baseline_flushes = peer.flushed;
  }
  constexpr unsigned packets = 8;
  uint64_t stamp = audio_now.load() / 100;
  for (unsigned index = 0; index < packets; ++index) {
    auto packet = NonzeroPacket(441, stamp + uint64_t(index) * 100000);
    packet.flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
    audio_now = packet.timestamp * 100;
    source->Push(std::move(packet));
    WaitUntil(
        [&] {
          return service.Snapshot().submitted_frames ==
                 uint64_t(index + 1) * 441;
        },
        "A continuous capture flag must still submit the current audio");
  }
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.flushed == baseline_flushes && peer.accepted.size() == packets,
            "A discontinuity flag on a continuous capture clock must not "
            "flush the receiver");
    for (unsigned index = 0; index < packets; ++index)
      Require(peer.accepted[index].timing.sample_index == index * 441 &&
                  peer.accepted[index].timing.timeline_epoch ==
                      peer.accepted.front().timing.timeline_epoch,
              "Flagged continuous audio stays on one receiver timeline");
  }
  auto continuous = service.Snapshot();
  Require(!continuous.discontinuity_reasons.contains("capture-discontinuity") &&
              !continuous.discontinuity_reasons.contains("source-clock-jump"),
          "A continuous capture flag is not a timeline break");
  auto jump = NonzeroPacket(441, stamp + packets * 100000 + 1000000);
  audio_now = jump.timestamp * 100;
  source->Push(std::move(jump));
  WaitUntil(
      [&] {
        auto reasons = service.Snapshot().discontinuity_reasons;
        auto found = reasons.find("source-clock-jump");
        return found != reasons.end() && found->second == 1 &&
               service.Snapshot().submitted_frames ==
                   uint64_t(packets + 1) * 441;
      },
      "A capture clock break still flushes once and anchors the new audio");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.flushed == baseline_flushes + 1 &&
                peer.accepted.back().timing.sample_index == 0,
            "The receiver timeline restarts once for a real capture break");
  }
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "Flagged continuous audio shuts down cleanly");
}
void DevicePositionClockScenario(std::filesystem::path const& root,
                                 std::string const& id) {
  auto position_root = root / L"device-position-clock";
  auto configuration = Save(position_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "ultra";
  configuration.settings.silence_timeout = 0;
  SettingsStore(position_root).Save(configuration);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  std::atomic<uint64_t> audio_now{50000000000};
  auto hooks = VirtualAudioServices(source);
  hooks.audio_now = [&] { return audio_now.load(); };
  StreamCoordinator service(peer, position_root, false, hooks);
  Require(Complete(service.Connect(id)).status == CommandStatus::applied,
          "Continuous device position connects before audio is submitted");
  unsigned baseline_flushes;
  {
    std::lock_guard lock(peer.mutex);
    baseline_flushes = peer.flushed;
  }
  constexpr unsigned packets = 4;
  uint64_t origin = audio_now.load() / 100;
  uint64_t position = 1000;
  for (unsigned index = 0; index < packets; ++index) {
    auto packet = NonzeroPacket(441, origin + uint64_t(index) * 100000);
    if (index) packet.timestamp += 300000;
    if (index == 2) packet.flags = AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
    packet.position = position;
    position += packet.frames;
    audio_now = (origin + uint64_t(index) * 100000) * 100;
    source->Push(std::move(packet));
    WaitUntil(
        [&] {
          return service.Snapshot().submitted_frames ==
                 uint64_t(index + 1) * 441;
        },
        "Continuous device frames must submit despite a jumped capture "
        "clock");
  }
  {
    auto reasons = service.Snapshot().discontinuity_reasons;
    std::lock_guard lock(peer.mutex);
    Require(peer.flushed == baseline_flushes &&
                peer.accepted.size() == packets &&
                !reasons.contains("source-clock-jump") &&
                !reasons.contains("capture-timestamp-unavailable"),
            "Continuous device positions do not flush a jumped or missing "
            "capture timestamp");
    for (unsigned index = 0; index < packets; ++index)
      Require(peer.accepted[index].timing.sample_index == index * 441 &&
                  peer.accepted[index].timing.timeline_epoch ==
                      peer.accepted.front().timing.timeline_epoch,
              "Continuous device frames stay on one receiver timeline");
  }
  auto gap = NonzeroPacket(441, origin + packets * 100000 + 300000);
  gap.position = position + 441;
  audio_now = gap.timestamp * 100;
  source->Push(std::move(gap));
  WaitUntil(
      [&] {
        auto reasons = service.Snapshot().discontinuity_reasons;
        auto found = reasons.find("source-clock-jump");
        return found != reasons.end() && found->second == 1 &&
               service.Snapshot().submitted_frames ==
                   uint64_t(packets + 1) * 441;
      },
      "A device position gap still flushes once and submits the new audio");
  {
    std::lock_guard lock(peer.mutex);
    Require(
        peer.flushed == baseline_flushes + 1 &&
            peer.accepted.back().timing.sample_index == 0,
        "The receiver timeline restarts once for a real device position gap");
  }
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "Device position continuity shuts down cleanly");
}
void CapturePacketLossScenario(std::filesystem::path const& root,
                               std::string const& id) {
  auto loss_root = root / L"capture-packet-loss";
  auto configuration = Save(loss_root);
  ++configuration.settings.revision;
  configuration.settings.latency_mode = "ultra";
  configuration.settings.silence_timeout = 0;
  SettingsStore(loss_root).Save(configuration);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  std::atomic<uint64_t> now{50000000000};
  auto hooks = VirtualAudioServices(source);
  hooks.audio_now = [&] { return now.load(); };
  StreamCoordinator service(peer, loss_root, false, hooks);
  Require(Complete(service.Connect(id)).status == CommandStatus::applied,
          "Prepare continuous playback with bounded capture loss");
  unsigned baseline;
  {
    std::lock_guard lock(peer.mutex);
    baseline = peer.flushed;
  }
  uint64_t position = 1000, elapsed{};
  for (unsigned index = 0; index < 150; ++index) {
    if (index) {
      position += 441;
      elapsed += 441;
    }
    auto packet = NonzeroPacket(441, 500000000 + elapsed * 10000000 / 44100);
    packet.position = position;
    if (index) packet.flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
    now = packet.timestamp * 100;
    source->Push(std::move(packet));
    position += 441;
    elapsed += 441;
    WaitUntil([&] { return service.Snapshot().submitted_frames == elapsed; },
              "Current audio and bounded missing intervals must reach the "
              "receiver");
  }
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.flushed == baseline && peer.created == 1 &&
                !service.Snapshot().discontinuity_reasons.contains(
                    "source-clock-jump"),
            "Repeated capture loss must not trigger remote FLUSH or reconnect");
  }
  Require(service.Shutdown(true).get().status == CommandStatus::applied,
          "Capture loss streaming shuts down cleanly");
}
void SourceErrorScenario(std::filesystem::path const& root,
                         std::string const& id) {
  auto failure_root = root / L"source-error";
  Save(failure_root);
  VirtualReceiver peer;
  auto source = std::make_shared<VirtualAudioState>();
  std::atomic<uint64_t> clock{MonotonicNs()};
  auto clocks = VirtualAudioServices(source);
  clocks.silence_now = [&] { return clock.load(); };
  StreamCoordinator service(peer, failure_root, false, clocks);
  Require(Complete(service.Connect(id)).status == CommandStatus::applied,
          "Prepare a virtual capture failure");
  {
    std::lock_guard lock(source->mutex);
    source->fail_next = true;
  }
  WaitUntil(
      [&] { return service.Snapshot().source.availability == "unavailable"; },
      "Capture failure was misclassified as silence");
  Require(!service.Snapshot().source.error.empty(),
          "A failed source exposes its error instead of claiming standby");
  service.Shutdown(true).get();
}
}  // namespace Win2Air::Tests
