#include "presentation_standby_scenarios.h"

namespace Win2Air::Tests {

void ConversionChainScenario(std::filesystem::path const& root,
                             SettingsSnapshot const& saved) {
  VirtualReceiver receiver;
  auto audio = std::make_shared<VirtualAudioState>();
  audio->period.default_frames = 1323;
  std::atomic<uint64_t> clock{MonotonicNs()};
  auto services = VirtualAudioServices(audio);
  std::mutex audio_log_mutex;
  std::vector<std::string> audio_logs;
  services.log_sink = [&](LogRecord const& record) {
    if (record.event != LogEvent::audio_timeline) return;
    std::lock_guard lock(audio_log_mutex);
    audio_logs.push_back(record.detail);
  };
  WasapiCapture* observed_capture{};
  services.capture = [&] {
    auto capture = std::make_unique<WasapiCapture>(
        [audio] { return std::make_unique<VirtualCaptureBackend>(audio); });
    observed_capture = capture.get();
    return capture;
  };
  services.silence_now = [&] { return clock.load(); };
  StreamCoordinator coordinator(receiver, root, false, services);
  auto id = saved.receivers.front().receiver_id;
  Require(Complete(coordinator.Connect(id)).status == CommandStatus::applied,
          "Connect the virtual receiver with a real software conversion chain");
  Require(Complete(coordinator.SetLatency("custom", 20)).status ==
                  CommandStatus::failed &&
              coordinator.Snapshot().preferences.latency_mode == "balanced" &&
              receiver.presentations.size() == 1 &&
              receiver.presentations.front().base_ms == 1000,
          "Unsupported capture period rejects the budget before changing the "
          "receiver");
  Require(Complete(coordinator.SetLatency("custom", 400)).status ==
                  CommandStatus::applied &&
              coordinator.Snapshot().timing.effective_ms == 400 &&
              receiver.created == 2,
          "Switching to custom disconnects and reconnects once");
  unsigned sessions = 2;
  for (int budget : {300, 250, 200, 150, 120, 100}) {
    ++sessions;
    Require(Complete(coordinator.SetLatency("custom", budget)).status ==
                    CommandStatus::applied &&
                coordinator.Snapshot().timing.effective_ms == budget &&
                receiver.created == sessions,
            "A custom delay change disconnects and reconnects once");
  }
  ++sessions;
  Require(Complete(coordinator.SetLatency("low", 1000)).status ==
                  CommandStatus::applied &&
              coordinator.Snapshot().timing.base_ms == 120 &&
              receiver.created == sessions &&
              observed_capture->BufferCapacityBytes() == 19404,
          "Latency control changes the actual receiver timing before reporting "
          "applied");
  Require(Complete(coordinator.SetOffset(id, 120)).status ==
                  CommandStatus::applied &&
              coordinator.Snapshot().preferences.latency_mode == "low" &&
              coordinator.Snapshot().timing.offset_ms == 120 &&
              coordinator.Snapshot().timing.base_ms == 120,
          "Positive compensation moves the active sender timeline without a "
          "new connection");
  auto before = coordinator.Snapshot().preferences.revision;
  Require(Complete(coordinator.SetOffset(id, -1000)).status ==
                  CommandStatus::failed &&
              coordinator.Snapshot().preferences.revision == before,
          "Illegal effective budget retains the applied settings and "
          "transaction revision");
  unsigned retargets;
  {
    std::lock_guard lock(receiver.mutex);
    receiver.reject_presentation = true;
    receiver.hold_presentation = true;
    receiver.interrupt_rejected_presentation = true;
    retargets = receiver.retarget_requests;
  }
  ++sessions;
  Require(Complete(coordinator.SetLatency("stable", 1000)).status ==
                  CommandStatus::applied &&
              coordinator.Snapshot().preferences.latency_mode == "stable" &&
              coordinator.Snapshot().timing.base_ms == 2000 &&
              coordinator.Snapshot().timing.offset_ms == 120 &&
              coordinator.Snapshot().session->session->state ==
                  SessionState::ready &&
              receiver.created == sessions,
          "Switching to stable disconnects and reconnects once");
  {
    std::lock_guard lock(receiver.mutex);
    Require(
        receiver.retarget_requests == retargets && receiver.created == sessions,
        "The new mode opens one session");
    receiver.reject_presentation = false;
    receiver.hold_presentation = false;
    receiver.interrupt_rejected_presentation = false;
  }
  ++sessions;
  Require(Complete(coordinator.SetLatency("custom", 750)).status ==
                  CommandStatus::applied &&
              coordinator.Snapshot().timing.base_ms == 750 &&
              coordinator.Snapshot().timing.offset_ms == 120 &&
              receiver.created == sessions,
          "Switching from stable to custom disconnects and reconnects once");
  Require(Complete(coordinator.SetSilenceTimeout(10)).status ==
              CommandStatus::applied,
          "Silence timeout is persisted through the serial writer");
  Require(
      Complete(coordinator.SetOffset(id, -120)).status ==
              CommandStatus::applied &&
          coordinator.Snapshot().timing.effective_ms == 630,
      "Legal negative compensation applies exactly once to the active target");
  Require(
      Complete(coordinator.SetOffset(id, 120)).status == CommandStatus::applied,
      "Restore the saved compensation for the standby checks");
  auto settings_file = root / L"settings.json";
  Require(SetFileAttributesW(settings_file.c_str(), FILE_ATTRIBUTE_READONLY),
          "Prepare an independent settings save failure");
  auto failed_save = Complete(coordinator.SetLatency("balanced", 1000));
  Require(SetFileAttributesW(settings_file.c_str(), FILE_ATTRIBUTE_NORMAL),
          "Release the settings failure fixture");
  Require(
      failed_save.status == CommandStatus::failed &&
          coordinator.Snapshot().preferences.latency_mode == "custom" &&
          coordinator.Snapshot().preferences.custom_ms == 750 &&
          coordinator.Snapshot().timing.base_ms == 750 &&
          coordinator.Snapshot().timing.offset_ms == 120 &&
          coordinator.Snapshot().session->session->state == SessionState::ready,
      "Save failure restores the sender timeline and keeps the session");
  Require(Complete(coordinator.SetLocalPlayback(false)).status ==
              CommandStatus::applied,
          "Prepare a local mute lease on the virtual endpoint");
  unsigned before_quiet_flush;
  uint64_t before_quiet_capture = coordinator.Snapshot().captured_frames;
  {
    std::lock_guard lock(receiver.mutex);
    receiver.injected_state = std::pair<uint64_t, sender_state>{
        coordinator.Snapshot().session->session->generation, SENDER_STREAMING};
    before_quiet_flush = receiver.flushed;
  }
  WaitUntil(
      [&] {
        return coordinator.Snapshot().session->session->state ==
               SessionState::streaming;
      },
      "The virtual receiver reports an actively streaming session");
  auto quiet_passage = NonzeroPacket(4410);
  auto quiet_timestamp = quiet_passage.timestamp;
  std::fill(quiet_passage.bytes.begin(), quiet_passage.bytes.end(), uint8_t{0});
  audio->Push(std::move(quiet_passage));
  WaitUntil(
      [&] {
        auto value = coordinator.Snapshot();
        return value.captured_frames >= before_quiet_capture + 4410 &&
               value.session->session->standby_state ==
                   StandbyState::silence_pending;
      },
      "A quiet passage remains in the streaming session before the timeout");
  Require(std::wstring_view(ConnectedStatusKey(
              coordinator.Snapshot().session->session->state,
              StandbyState::silence_pending)) == L"StatusPlaying",
          "A quiet passage keeps playing status without a pause event");
  auto returning_sound = NonzeroPacket(4410, quiet_timestamp + 1000000);
  returning_sound.position = 4410;
  audio->Push(std::move(returning_sound));
  WaitUntil(
      [&] {
        auto value = coordinator.Snapshot();
        return value.captured_frames >= before_quiet_capture + 8820 &&
               value.session->session->standby_state == StandbyState::active;
      },
      "Sound after a quiet passage continues on the same active session");
  {
    std::lock_guard lock(receiver.mutex);
    Require(
        receiver.flushed == before_quiet_flush,
        "Quiet passages and returning sound never flush the audio timeline");
  }
  clock += 11000000000;
  WaitUntil(
      [&] {
        auto state = coordinator.Snapshot().session->session;
        return state && state->standby_state == StandbyState::standby;
      },
      "A healthy source without new buffers never entered standby");
  {
    std::lock_guard lock(audio->mutex);
    Require(audio->levels.at("virtual-default").muted,
            "Standby retains the existing local playback lease");
  }
  unsigned flushed;
  unsigned destroyed;
  size_t received;
  {
    std::lock_guard lock(receiver.mutex);
    flushed = receiver.flushed;
    destroyed = receiver.destroyed;
    received = receiver.accepted.size();
  }
  auto silent = NonzeroPacket(4410);
  std::fill(silent.bytes.begin(), silent.bytes.end(), uint8_t{0});
  audio->Push(std::move(silent));
  WaitUntil(
      [&] {
        return coordinator.Snapshot().captured_frames >=
               before_quiet_capture + 13230;
      },
      "Standby stopped observing the source");
  {
    std::lock_guard lock(receiver.mutex);
    Require(
        receiver.accepted.size() == received && receiver.destroyed == destroyed,
        "Standby observes current input while sending no PCM and retaining "
        "the session");
  }
  audio->Push(NonzeroPacket(4410));
  WaitUntil(
      [&] {
        std::lock_guard lock(receiver.mutex);
        return receiver.accepted.size() > received;
      },
      "Voiced windows did not resume current converted audio");
  {
    std::lock_guard lock(receiver.mutex);
    Require(receiver.accepted.size() >= received + 2,
            "Wake audio was not sent");
    auto const& boundary = receiver.accepted[received];
    auto const& voice = receiver.accepted[received + 1];
    bool boundary_silent = boundary.pcm.size() == 352 * 4;
    for (auto value : boundary.pcm) boundary_silent = boundary_silent && !value;
    bool voiced = false;
    for (auto value : voice.pcm) voiced = voiced || value;
    Require(receiver.flushed == flushed + 1 &&
                boundary.timing.sample_index == 0 && boundary_silent &&
                voice.timing.sample_index == 352 && voiced &&
                voice.timing.timeline_epoch == boundary.timing.timeline_epoch,
            "Wakeup flushes once, sends the discarded RTP boundary as silence, "
            "then the voiced audio");
    auto spacing = voice.timing.capture_monotonic_ns -
                   boundary.timing.capture_monotonic_ns;
    Require(voice.timing.capture_monotonic_ns >
                    boundary.timing.capture_monotonic_ns &&
                spacing == 352ull * 10000000 / 44100 * 100,
            "The discarded boundary is one packet before the voiced audio on "
            "the resumed clock");
  }
  {
    std::lock_guard lock(audio_log_mutex);
    bool parked = false, resumed = false;
    for (auto const& line : audio_logs) {
      parked = parked || line == "park reason=silence-standby";
      resumed = resumed || line.rfind("resume origin_100ns=", 0) == 0;
    }
    Require(parked && resumed,
            "Standby and resume record the park reason and the rebased clock");
  }
  Require(Complete(coordinator.SetSilenceTimeout(0)).status ==
              CommandStatus::applied,
          "Never disables automatic silence standby");
  clock += 100000000000;
  Complete(coordinator.SetLifecycle(true, false, false));
  Require(coordinator.Snapshot().session->session->standby_state !=
              StandbyState::standby,
          "Never remains active despite an advanced no-buffer clock");
  Require(Complete(coordinator.SetSilenceTimeout(15)).status ==
              CommandStatus::failed,
          "Unsupported timeout never replaces the saved Never choice");
  auto result = coordinator.Shutdown(true);
  Require(
      result.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
          result.get().status == CommandStatus::applied,
      "Exit retains bounded cleanup after timing and standby operations");
  auto restarted = SettingsStore(root).Load();
  {
    std::lock_guard lock(audio->mutex);
    Require(!audio->levels.at("virtual-default").muted,
            "Final termination restores the endpoint owned during standby");
  }
  Require(restarted.settings.latency_mode == "custom" &&
              restarted.settings.custom_ms == 750 &&
              restarted.receivers.front().offset_ms == 120 &&
              restarted.settings.silence_timeout == 0,
          "Restart retains confirmed presentation, compensation and silence "
          "policy");
  {
    auto canceled_root = root / L"presentation-canceled";
    Save(canceled_root);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator service(peer, canceled_root, false,
                              VirtualAudioServices(source));
    Require(Complete(service.Connect(id)).status == CommandStatus::applied,
            "Prepare a connected latency change");
    auto change = service.SetLatency("stable", 1000);
    Require(
        Complete(std::move(change)).status == CommandStatus::applied &&
            service.Snapshot().timing.base_ms == 2000 &&
            service.Snapshot().preferences.latency_mode == "stable" &&
            service.Snapshot().session->session->state == SessionState::ready,
        "Switching to stable disconnects and reconnects once");
    auto disconnect = service.Disconnect();
    Require(Complete(std::move(disconnect)).status == CommandStatus::applied &&
                service.Snapshot().preferences.latency_mode == "stable",
            "Disconnect keeps the latency mode applied on the sender timeline");
    auto exit = service.Shutdown(true);
    exit.get();
  }
  {
    auto rollback_root = root / L"presentation-rollback-error";
    Save(rollback_root);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator service(peer, rollback_root, false,
                              VirtualAudioServices(source));
    Require(Complete(service.Connect(id)).status == CommandStatus::applied &&
                Complete(service.SetLocalPlayback(false)).status ==
                    CommandStatus::applied,
            "Prepare rollback failure with a local mute lease");
    Require(Complete(service.SetLatency("custom", 1000)).status ==
                    CommandStatus::applied &&
                service.Snapshot().timing.base_ms == 1000 &&
                service.Snapshot().preferences.latency_mode == "custom",
            "Custom mode disconnects and reconnects once");
    auto revision = service.Snapshot().preferences.revision;
    auto path = rollback_root / L"settings.json";
    Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY),
            "Reject persistence of the next custom delay");
    auto failure = Complete(service.SetLatency("custom", 400));
    Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL),
            "Release the rollback save failure fixture");
    Require(
        failure.status == CommandStatus::failed &&
            service.Snapshot().session->session &&
            service.Snapshot().session->session->state == SessionState::ready &&
            service.Snapshot().timing.base_ms == 1000 &&
            service.Snapshot().preferences.latency_mode == "custom" &&
            service.Snapshot().preferences.custom_ms == 1000 &&
            service.Snapshot().preferences.revision == revision,
        "An unsaved custom delay keeps the current session");
    {
      std::lock_guard lock(peer.mutex);
      Require(peer.created == 2 && peer.presentation_requests == 0 &&
                  peer.retarget_requests == 0,
              "The new delay waits for a saved budget before reconnecting");
    }
    {
      std::lock_guard lock(source->mutex);
      Require(source->levels.at("virtual-default").muted,
              "The active session keeps the local playback lease");
    }
    Require(
        SettingsStore(rollback_root).Load().settings.latency_mode == "custom" &&
            SettingsStore(rollback_root).Load().settings.custom_ms == 1000,
        "Failed persistence retains the last confirmed budget on disk");
    auto exit = service.Shutdown(true);
    Require(
        exit.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
            exit.get().status == CommandStatus::applied,
        "Termination remains safe after failed rollback cleanup");
  }
  {
    auto continuous_root = root / L"ultra-continuous";
    auto configuration = Save(continuous_root);
    ++configuration.settings.revision;
    configuration.settings.latency_mode = "low";
    configuration.settings.silence_timeout = 0;
    SettingsStore(continuous_root).Save(configuration);
    auto source = std::make_shared<VirtualAudioState>();
    VirtualReceiver peer;
    auto hooks = VirtualAudioServices(source);
    std::atomic<uint64_t> audio_now{1000000000};
    hooks.audio_now = [&] { return audio_now.load(); };
    hooks.transform = [] {
      auto transform = winrt::make_self<TransformFixture>();
      transform->batch_input_count = 2;
      return transform.as<IMFTransform>();
    };
    StreamCoordinator service(peer, continuous_root, false, hooks);
    Require(Complete(service.Connect(id)).status == CommandStatus::applied,
            "Continuous ultra streaming starts with a batching transform");
    unsigned baseline_flushes;
    {
      std::lock_guard lock(peer.mutex);
      baseline_flushes = peer.flushed;
      peer.backpressure = 1;
    }
    auto start = audio_now.load() / 100 + 500000;
    for (unsigned index = 0; index < 30; ++index) {
      source->Push(NonzeroPacket(441, start + uint64_t(index) * 100000));
      WaitUntil(
          [&] {
            return service.Snapshot().captured_frames == (index + 1) * 441;
          },
          "Continuous PCM must reach the batching transform");
      if (index % 2) {
        try {
          if (index == 1)
            WaitUntil([&] { return service.Snapshot().future_hold_max_ns > 0; },
                      "The virtual clock must hold the future batch");
          audio_now = (start + uint64_t(index - 1) * 100000) * 100;
          WaitUntil(
              [&] {
                return service.Snapshot().submitted_frames == index * 441;
              },
              "The first split segment submits while its future tail waits");
          audio_now = (start + uint64_t(index) * 100000) * 100;
          WaitUntil(
              [&] {
                return service.Snapshot().submitted_frames == (index + 1) * 441;
              },
              "Batched MFT output must submit every captured frame");
        } catch (std::exception const& failure) {
          auto state = service.Snapshot();
          auto detail = std::string(failure.what()) +
                        ": input=" + std::to_string(index) +
                        " captured=" + std::to_string(state.captured_frames) +
                        " submitted=" + std::to_string(state.submitted_frames) +
                        " source=" + state.source.availability +
                        " source-error=" + state.source.error;
          if (state.session)
            detail += " command=" + state.session->last_command.message;
          for (auto const& [reason, count] : state.discontinuity_reasons)
            detail += " " + reason + "=" + std::to_string(count);
          {
            std::lock_guard lock(peer.mutex);
            detail += " attempts=" + std::to_string(peer.attempted.size()) +
                      " accepted=" + std::to_string(peer.accepted.size()) +
                      " flushes=" + std::to_string(peer.flushed);
          }
          throw std::runtime_error(detail);
        }
      }
    }
    {
      std::lock_guard lock(peer.mutex);
      Require(peer.flushed == baseline_flushes && peer.accepted.size() == 30,
              "Batched conversion preserves normal PCM without queue FLUSH");
      Require(peer.attempted.size() == 31 &&
                  peer.attempted[0].pcm == peer.attempted[1].pcm &&
                  peer.attempted[0].timing.sample_index ==
                      peer.attempted[1].timing.sample_index,
              "A blocked split segment is retried intact before its retained "
              "tail is submitted");
      for (size_t index = 0; index < peer.accepted.size(); ++index) {
        auto const& block = peer.accepted[index];
        Require(
            block.pcm.size() == 1764 &&
                block.timing.sample_index == index * 441 &&
                block.timing.capture_monotonic_ns ==
                    (start + index * 100000) * 100,
            "Splitting output keeps ordered samples and capture timestamps");
      }
    }
    Require(service.Snapshot().future_hold_max_ns > 0,
            "Future-timestamped batches retain their remaining segments until "
            "eligible without forcing a FLUSH");
    audio_now = (start + 30 * 100000) * 100 + 121000000;
    for (unsigned index = 30; index < 32; ++index) {
      source->Push(NonzeroPacket(441, start + uint64_t(index) * 100000));
      WaitUntil(
          [&] {
            return service.Snapshot().captured_frames == (index + 1) * 441;
          },
          "Expired input still reaches the production scheduling check");
    }
    WaitUntil(
        [&] {
          auto state = service.Snapshot();
          auto expired = state.discontinuity_reasons.find("expired-capture");
          return expired != state.discontinuity_reasons.end() &&
                 expired->second == 1;
        },
        "Expired audio must FLUSH instead of bypassing the playback deadline");
    {
      std::lock_guard lock(peer.mutex);
      Require(peer.flushed == baseline_flushes + 1 &&
                  peer.accepted.size() == 30 && peer.attempted.size() == 31,
              "Expired batches discard their tail before submitting any PCM");
    }
    Require(service.Shutdown(true).get().status == CommandStatus::applied,
            "Continuous ultra streaming ends cleanly");
  }
}
}  // namespace Win2Air::Tests
