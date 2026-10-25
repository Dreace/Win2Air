#include "../../src/Streaming/StreamCoordinator.h"
#include "../../src/UI/ViewModels/ReceiverViewModel.h"
#include "../../src/UI/ViewModels/TrayViewModel.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
using namespace Win2Air;
using namespace Win2Air::Tests;
namespace {
CommandResult Complete(std::future<CommandResult> value) {
  Require(value.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "Recovery command did not resolve within its deadline");
  return value.get();
}
void Exit(StreamCoordinator& service) {
  auto exit = service.Shutdown(true);
  Require(exit.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
              exit.get().status == CommandStatus::applied,
          "Recovery cleanup must finish and restore its owned resources");
}
SettingsSnapshot Save(std::filesystem::path const& root) {
  SettingsStore store(root);
  auto value = store.Load();
  value.settings.revision = 1;
  value.settings.latency_mode = "custom";
  value.settings.custom_ms = 750;
  for (auto id : {"AA:BB:CC:DD:EE:FF", "11:22:33:44:55:66"}) {
    ReceiverRecord receiver;
    receiver.receiver_id = id;
    receiver.name = receiver.model = "Virtual recovery receiver";
    receiver.features =
        (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
    receiver.manual_endpoint = ReceiverEndpoint{"127.0.0.1", "127.0.0.1", 7000};
    receiver.manual_endpoint->interface_index = 5;
    receiver.offset_ms = 120;
    receiver.volume = 0.4;
    value.receivers.push_back(receiver);
  }
  store.Save(value);
  return value;
}
void Fault(VirtualReceiver& receiver, sender_result code = SENDER_NETWORK,
           int peer = 0) {
  std::lock_guard lock(receiver.mutex);
  receiver.session_failure = std::pair(code, peer);
}
void Waiting(StreamCoordinator& service) {
  WaitUntil([&] { return service.Snapshot().reconnect.has_value(); },
            "Transient interruption never scheduled recovery");
}
void Ready(StreamCoordinator& service) {
  WaitUntil(
      [&] {
        auto value = service.Snapshot();
        return value.session->session && !value.reconnect &&
               (value.session->session->state == SessionState::ready ||
                value.session->session->state == SessionState::streaming);
      },
      "Retry did not establish a ready receiver");
}
}  // namespace
void TestRecoveryFaults(wchar_t const* directory) {
  auto root = std::filesystem::path(directory);
  auto saved = Save(root);
  auto id = saved.receivers.front().receiver_id;
  VirtualReceiver receiver;
  auto audio = std::make_shared<VirtualAudioState>();
  std::atomic<uint64_t> clock{MonotonicNs()};
  auto services = VirtualAudioServices(audio);
  services.recovery_now = [&] { return clock.load(); };
  services.silence_now = [&] { return clock.load(); };
  StreamCoordinator service(receiver, root, false, services);
  Require(Complete(service.Connect(id)).status == CommandStatus::applied &&
              Complete(service.SetLocalPlayback(false)).status ==
                  CommandStatus::applied,
          "Establish recovery with a nondefault budget and local mute lease");
  audio->Push(NonzeroPacket(4410));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 4410; },
            "Initial current audio was not submitted");
  auto old_generation = service.Snapshot().session->session->generation;
  Fault(receiver);
  Waiting(service);
  auto state = service.Snapshot();
  auto views = ReceiverViews(state);
  auto tray = ProjectTray(state);
  Require(state.session->intent.desired_connected && views.front().active &&
              views.front().state == SessionState::recovering &&
              views.front().retry_seconds == 2u &&
              tray.state == SessionState::recovering && !tray.adjustable,
          "Window and tray project actual recovery and cancellation intent");
  {
    std::lock_guard lock(audio->mutex);
    Require(audio->levels.at("virtual-default").muted,
            "Network recovery retains the original local mute lease");
  }
  clock = state.reconnect->next_due_monotonic;
  Ready(service);
  {
    std::lock_guard lock(receiver.mutex);
    Require(receiver.created == 2 && receiver.destroyed == 1 &&
                receiver.initial_volumes.back() == 40 &&
                receiver.presentations.back().base_ms == 750 &&
                receiver.presentations.back().offset_ms == 120,
            "Reconnect restores effective volume and presentation after "
            "destroying the old runtime");
    receiver.injected_state = std::pair(old_generation, SENDER_FAILED);
  }
  WaitUntil(
      [&] {
        std::lock_guard lock(receiver.mutex);
        return receiver.late_states_delivered != 0;
      },
      "Old receiver callback fixture was not delivered");
  Require(!service.Snapshot().reconnect,
          "A stale generation cannot revive the old recovery failure");
  audio->Push(NonzeroPacket(4410, MonotonicNs() / 100 - 100000000));
  WaitUntil(
      [&] {
        return service.Snapshot().discontinuity_reasons.contains(
            "expired-capture");
      },
      "Expired recovery audio was not discarded");
  audio->Push(NonzeroPacket(4410));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 8820; },
            "Recovery did not submit newly captured audio");
  {
    std::lock_guard lock(receiver.mutex);
    Require(receiver.accepted.back().timing.sample_index == 0,
            "Recovered audio starts at a fresh anchor after expired content");
  }
  service.NotifySystemEvent(SystemEvent::network_changed, 9);
  Complete(service.SetLifecycle(true, false, false));
  Require(!service.Snapshot().reconnect,
          "An unrelated known interface does not interrupt the receiver");
  {
    std::lock_guard lock(receiver.mutex);
    receiver.hold_volume = true;
  }
  auto held_volume = service.SetReceiverVolume(id, 0.8);
  WaitUntil([&] { return service.Snapshot().volume_pending; },
            "Prepare an occupied owner to coalesce interface notifications");
  service.NotifySystemEvent(SystemEvent::network_changed, 5);
  service.NotifySystemEvent(SystemEvent::network_changed, 9);
  {
    std::lock_guard lock(receiver.mutex);
    receiver.hold_volume = false;
  }
  Require(Complete(std::move(held_volume)).status == CommandStatus::applied,
          "Release the owner after simultaneous interface changes");
  Waiting(service);
  Require(service.Snapshot().reconnect->attempt == 1,
          "Success resets the next fault to the initial delay");
  Require(Complete(service.SetReceiverMuted(id, true)).status ==
              CommandStatus::applied,
          "Recovery waiting can persist a new muted preference");
  clock = service.Snapshot().reconnect->next_due_monotonic;
  Ready(service);
  {
    std::lock_guard lock(receiver.mutex);
    Require(receiver.initial_volumes.back() == 0,
            "A retry applies the current muted preference");
  }
  Require(
      Complete(service.SetSilenceTimeout(10)).status == CommandStatus::applied,
      "Prepare silence standby before a receiver interruption");
  clock += 11000000000;
  WaitUntil(
      [&] {
        return service.Snapshot().session->session->standby_state ==
               StandbyState::standby;
      },
      "The silent receiver never entered standby");
  Fault(receiver);
  Waiting(service);
  clock = service.Snapshot().reconnect->next_due_monotonic;
  Ready(service);
  Require(service.Snapshot().session->session->standby_state ==
              StandbyState::standby,
          "A disconnected silent receiver retains standby on reconnection");
  audio->Push(NonzeroPacket(4410));
  WaitUntil(
      [&] {
        return service.Snapshot().session->session->standby_state !=
               StandbyState::standby;
      },
      "Current voiced input did not resume recovered standby");
  service.NotifySystemEvent(SystemEvent::suspend);
  WaitUntil(
      [&] {
        return service.Snapshot().session->session->state ==
               SessionState::recovering;
      },
      "Suspend did not stop the active receiver");
  auto opened = [&] {
    std::lock_guard lock(receiver.mutex);
    return receiver.created;
  }();
  clock += 100000000000;
  Complete(service.SetLifecycle(true, false, false));
  {
    std::lock_guard lock(receiver.mutex);
    Require(receiver.created == opened, "Suspended services never retry");
  }
  service.NotifySystemEvent(SystemEvent::resume);
  Waiting(service);
  clock = service.Snapshot().reconnect->next_due_monotonic;
  Ready(service);
  Fault(receiver);
  Waiting(service);
  Require(Complete(service.SetAutoReconnect(false)).status ==
                  CommandStatus::applied &&
              !service.Snapshot().reconnect &&
              !service.Snapshot().session->intent.desired_connected,
          "Disabling recovery cancels waiting and ends its connection intent");
  clock += 100000000000;
  Require(Complete(service.SetAutoReconnect(true)).status ==
                  CommandStatus::applied &&
              !service.Snapshot().reconnect,
          "Enabling recovery does not restore a canceled connection intent");
  Require(Complete(service.Connect(id)).status == CommandStatus::applied,
          "Explicit user connection starts a new allowed intent");
  Fault(receiver, SENDER_AUTHENTICATION, 470);
  WaitUntil(
      [&] { return !service.Snapshot().session->intent.desired_connected; },
      "Authentication invalidation did not stop automatic recovery");
  Require(!service.Snapshot().reconnect &&
              ReceiverViews(service.Snapshot()).front().failure_kind ==
                  "authentication",
          "Authentication failure exposes required user action without retry");
  Require(Complete(service.Connect(id, SecureBuffer(std::vector<uint8_t>{
                                           '1', '2', '3', '4'})))
                  .status == CommandStatus::applied,
          "Explicit pairing can recover after failed credentials");
  Fault(receiver);
  Waiting(service);
  {
    std::lock_guard lock(receiver.mutex);
    receiver.hold_pairing = true;
  }
  clock = service.Snapshot().reconnect->next_due_monotonic;
  WaitUntil(
      [&] {
        return service.Snapshot().session->session->state ==
               SessionState::pairing;
      },
      "The controlled retry never entered authentication");
  auto switch_id = saved.receivers.back().receiver_id;
  auto switch_intent = service.Connect(switch_id);
  {
    std::lock_guard lock(receiver.mutex);
    receiver.hold_pairing = false;
  }
  Require(Complete(std::move(switch_intent)).status == CommandStatus::applied &&
              service.Snapshot().session->session->receiver_id == switch_id &&
              !service.Snapshot().reconnect,
          "Latest receiver selection cancels an in-flight old retry");
  Fault(receiver);
  Waiting(service);
  Require(Complete(service.Disconnect()).status == CommandStatus::applied &&
              service.Snapshot().session->intent.suppressed_this_run,
          "Explicit disconnect suppresses automatic connection for this run");
  service.NotifySystemEvent(SystemEvent::resume);
  service.NotifySystemEvent(SystemEvent::network_changed);
  clock += 100000000000;
  Complete(service.SetLifecycle(true, false, false));
  Require(!service.Snapshot().reconnect && !service.Snapshot().session->session,
          "Network and power events cannot undo explicit disconnection");
  Exit(service);
  {
    std::lock_guard lock(audio->mutex);
    Require(!audio->levels.at("virtual-default").muted,
            "Final cancellation restores local playback");
  }
  {
    auto path = root / L"repeated-network-failure";
    Save(path);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    auto injected = VirtualAudioServices(source);
    injected.recovery_now = [&] { return clock.load(); };
    StreamCoordinator retry(peer, path, false, injected);
    Require(Complete(retry.Connect(id)).status == CommandStatus::applied,
            "Prepare repeated connection failures");
    {
      std::lock_guard lock(peer.mutex);
      peer.connect_failure = SENDER_NETWORK;
    }
    Fault(peer);
    Waiting(retry);
    for (unsigned attempt = 2; attempt <= 4; ++attempt) {
      clock = retry.Snapshot().reconnect->next_due_monotonic;
      WaitUntil(
          [&] {
            auto waiting = retry.Snapshot().reconnect;
            return waiting && waiting->attempt == attempt;
          },
          "A failed retry did not advance its bounded backoff");
    }
    auto settings_path = path / L"settings.json";
    Require(SetFileAttributesW(settings_path.c_str(), FILE_ATTRIBUTE_READONLY),
            "Block persistence of the recovery toggle");
    auto failed_toggle = Complete(retry.SetAutoReconnect(false));
    Require(SetFileAttributesW(settings_path.c_str(), FILE_ATTRIBUTE_NORMAL),
            "Release the toggle persistence failure");
    Require(failed_toggle.status == CommandStatus::failed &&
                retry.Snapshot().preferences.auto_reconnect &&
                retry.Snapshot().reconnect->attempt == 4 &&
                retry.Snapshot().session->intent.desired_connected,
            "Failed toggle persistence retains recovery and its backoff "
            "without resetting the attempt count");
    {
      std::lock_guard lock(peer.mutex);
      peer.connect_failure = SENDER_OK;
      peer.hold_pairing = true;
    }
    clock = retry.Snapshot().reconnect->next_due_monotonic;
    WaitUntil(
        [&] {
          return retry.Snapshot().session->session->state ==
                 SessionState::pairing;
        },
        "A permitted retry never reached its controlled pairing wait");
    Require(
        Complete(retry.SetAutoReconnect(false)).status ==
                CommandStatus::applied &&
            !retry.Snapshot().session->session,
        "Disabling recovery cancels an in-flight retry and releases ownership");
    Exit(retry);
    Require(!SettingsStore(path).Load().settings.auto_reconnect,
            "The reconnect preference persists without a version increment");
  }
  {
    auto path = root / L"automatic-selection";
    auto value = Save(path);
    value.settings.selected_receiver_id = id;
    ++value.settings.revision;
    for (auto& record : value.receivers) {
      record.auto_connect = true;
      if (record.receiver_id == id) record.connect_established = true;
    }
    SettingsStore(path).Save(value);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator automatic(peer, path, false,
                                VirtualAudioServices(source));
    automatic.NotifySystemEvent(SystemEvent::network_changed);
    Ready(automatic);
    Require(automatic.Snapshot().session->session->receiver_id == id &&
                automatic.Snapshot().applied_volume == 40u,
            "Automatic connection targets only the saved selected receiver");
    Require(Complete(automatic.Disconnect()).status == CommandStatus::applied,
            "Cancel automatically selected receiver");
    automatic.NotifySystemEvent(SystemEvent::network_changed);
    Complete(automatic.SetLifecycle(true, false, false));
    Require(!automatic.Snapshot().session->intent.desired_connected,
            "Saved automatic flags cannot override explicit disconnect");
    Exit(automatic);
  }
  {
    auto path = root / L"specified-source";
    Save(path);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    auto injected = VirtualAudioServices(source);
    injected.recovery_now = [&] { return clock.load(); };
    StreamCoordinator specified(peer, path, false, injected);
    AudioSourceConfig config;
    config.follow_system_default = false;
    config.endpoint_id = "virtual-default";
    Require(
        Complete(specified.SetSource(config)).status ==
                CommandStatus::applied &&
            Complete(specified.Connect(id)).status == CommandStatus::applied,
        "Connect from an explicitly selected virtual source");
    auto device = source->devices.front();
    source->Remove(config.endpoint_id);
    Waiting(specified);
    Require(specified.Snapshot().source.availability == "unavailable" &&
                specified.Snapshot().source_config.endpoint_id ==
                    config.endpoint_id,
            "Source removal reports unavailable without selecting another "
            "endpoint");
    clock = specified.Snapshot().reconnect->next_due_monotonic;
    WaitUntil(
        [&] {
          return specified.Snapshot().reconnect &&
                 specified.Snapshot().reconnect->attempt == 2;
        },
        "An unavailable source did not retain bounded recovery");
    {
      std::lock_guard lock(source->mutex);
      source->devices.push_back(device);
    }
    source->Notify({device.id, DEVICE_STATE_ACTIVE});
    clock = specified.Snapshot().reconnect->next_due_monotonic;
    Ready(specified);
    Require(specified.Snapshot().source.endpoint_id == config.endpoint_id,
            "Restoration recaptures the specified source with a new epoch");
    Fault(peer, SENDER_CAPABILITY, 451);
    WaitUntil(
        [&] { return !specified.Snapshot().session->intent.desired_connected; },
        "Unsupported receiver capability did not stop retrying");
    Require(
        !specified.Snapshot().reconnect,
        "Capability failures require user action rather than network retries");
    Exit(specified);
  }
}
