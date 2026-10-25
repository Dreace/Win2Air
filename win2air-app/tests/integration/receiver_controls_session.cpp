#include "receiver_controls_support.h"

namespace Win2Air::Tests {

void ReceiverSessionScenario(std::filesystem::path const& root) {
  SavedReceivers(root);
  VirtualReceiver receiver;
  auto audio = std::make_shared<VirtualAudioState>();
  StreamCoordinator service(receiver, root, false, VirtualAudioServices(audio));
  Require(service.Snapshot().receivers.size() == 2 && receiver.created == 0,
          "Saved receivers never automatically create an audio session");
  Require(Complete(service.UpdateReceiver("11:22:33:44:55:66", "Edited name",
                                          true, true, false, 120))
                  .status == CommandStatus::applied,
          "Favorite, hidden and offset preferences save without connecting");
  Require(
      Complete(service.SetReceiverVolume("11:22:33:44:55:66", 0.4)).status ==
              CommandStatus::applied &&
          receiver.created == 0,
      "Inactive receiver volume is only persisted");
  auto visible = ReceiverViews(service.Snapshot());
  auto managed = ReceiverViews(service.Snapshot(), true);
  Require(visible.size() == 1 && managed.size() == 2 &&
              managed.front().receiver.receiver_id == "11:22:33:44:55:66" &&
              managed.front().health == "unknown",
          "Hidden filtering and favorite ordering preserve identity and "
          "unknown health");
  ReceiverSelection selection;
  Require(Complete(service.Connect("AA:BB:CC:DD:EE:FF")).status ==
              CommandStatus::applied,
          "Connect saved receiver by stable identity despite equal names");
  Require(receiver.initial_volumes.back() == 50,
          "Initial remote volume applies the saved receiver volume");
  Require(selection.Project(service.Snapshot(),
                            ReceiverViews(service.Snapshot(), true)) == 1 &&
              !selection.Project(service.Snapshot(),
                                 ReceiverViews(service.Snapshot(), true)),
          "Service selection projects identity once without overwriting user "
          "draft selection");
  {
    std::lock_guard lock(receiver.mutex);
    receiver.hold_volume = true;
  }
  auto volume = service.SetReceiverVolume("AA:BB:CC:DD:EE:FF", 0.6);
  WaitUntil([&] { return service.Snapshot().volume_pending; },
            "Remote volume command was not submitted");
  Require(volume.wait_for(std::chrono::milliseconds(0)) !=
                  std::future_status::ready &&
              service.Snapshot().receivers[0].volume == 0.5,
          "Submission cannot claim remote application or saved preference");
  audio->Push(NonzeroPacket(4410));
  WaitUntil(
      [&] {
        std::lock_guard lock(receiver.mutex);
        return !receiver.accepted.empty();
      },
      "Pending volume response must not stall capture and PCM submission");
  {
    std::lock_guard lock(receiver.mutex);
    receiver.hold_volume = false;
  }
  Require(Complete(std::move(volume)).status == CommandStatus::applied &&
              service.Snapshot().applied_volume == 60u,
          "Successful receiver response applies that receiver volume");
  Require(
      Complete(service.SetReceiverMuted("AA:BB:CC:DD:EE:FF", true)).status ==
              CommandStatus::applied &&
          service.Snapshot().applied_volume == 0u,
      "Receiver mute sends silence");
  Require(
      Complete(service.SetReceiverMuted("AA:BB:CC:DD:EE:FF", false)).status ==
              CommandStatus::applied &&
          service.Snapshot().applied_volume == 60u,
      "Unmute restores saved receiver volume");
  auto created = receiver.created;
  {
    std::lock_guard lock(receiver.mutex);
    receiver.reject_volume = true;
  }
  auto rejected = Complete(service.SetReceiverVolume("AA:BB:CC:DD:EE:FF", 0.8));
  Require(
      rejected.status == CommandStatus::failed && rejected.peer_code == 451 &&
          receiver.created == created &&
          service.Snapshot().applied_volume == 60u &&
          service.Snapshot().receivers[0].volume == 0.6,
      "Receiver rejection retains prior effective settings without reconnect");
  {
    std::lock_guard lock(receiver.mutex);
    receiver.reject_volume = false;
  }
  auto settings_file = root / L"settings.json";
  Require(SetFileAttributesW(settings_file.c_str(), FILE_ATTRIBUTE_READONLY),
          "Inject preference replacement failure");
  auto failed_save =
      Complete(service.SetReceiverVolume("AA:BB:CC:DD:EE:FF", 0.8));
  Require(SetFileAttributesW(settings_file.c_str(), FILE_ATTRIBUTE_NORMAL),
          "Release preference replacement failure");
  Require(failed_save.status == CommandStatus::failed &&
              service.Snapshot().receivers[0].volume == 0.6 &&
              service.Snapshot().applied_volume == 60u,
          "Failed save restores the prior remote volume and preference");
  Require(
      Complete(service.SetReceiverVolume("AA:BB:CC:DD:EE:FF", 1.01)).status ==
          CommandStatus::failed,
      "Out-of-range receiver volume fails before contacting receiver");
  Require(Complete(service.UpdateReceiver("AA:BB:CC:DD:EE:FF", "Active hidden",
                                          false, true, false, 0))
                      .status == CommandStatus::applied &&
              service.Snapshot().session->session,
          "Hiding the active receiver leaves its session running");
  Require(
      Complete(service.Connect("11:22:33:44:55:66")).status ==
              CommandStatus::applied &&
          receiver.created == created + 1 && receiver.destroyed == created &&
          service.Snapshot().session->session->receiver_id ==
              "11:22:33:44:55:66" &&
          receiver.initial_volumes.back() == 40,
      "Switch releases old ownership before applying new receiver settings");
  Require(selection.Project(service.Snapshot(),
                            ReceiverViews(service.Snapshot(), true)) == 0,
          "Card switch projects new favorite receiver into the destination "
          "selector");
  {
    std::lock_guard lock(receiver.mutex);
    receiver.reject_destroy = true;
  }
  auto switch_failed = Complete(service.Connect("AA:BB:CC:DD:EE:FF"));
  Require(switch_failed.status == CommandStatus::failed &&
              receiver.created == created + 1 &&
              service.Snapshot().session->intent.selected_receiver_id ==
                  "AA:BB:CC:DD:EE:FF",
          "Cleanup failure retains new selection and blocks another runtime");
  Require(selection.Project(service.Snapshot(),
                            ReceiverViews(service.Snapshot(), true)) == 1,
          "Failed card switch projects the retained new selection");
  {
    std::lock_guard lock(receiver.mutex);
    receiver.reject_destroy = false;
  }
  Require(Complete(service.Disconnect()).status == CommandStatus::applied,
          "Cleanup can retry its still-owned runtime");
  Require(!selection.Project(service.Snapshot(),
                             ReceiverViews(service.Snapshot(), true)),
          "Disconnected projection preserves the user's unsubmitted "
          "destination choice");
  Require(Complete(service.Connect("AA:BB:CC:DD:EE:FF")).status ==
              CommandStatus::applied,
          "Receiver can connect after cleanup retry");
  Require(
      selection.Project(service.Snapshot(),
                        ReceiverViews(service.Snapshot(), true)) == 1,
      "Reconnecting the same receiver still projects its newly active intent");
  Require(Complete(service.EditReceiverEndpoint("AA:BB:CC:DD:EE:FF",
                                                "127.0.0.1", 7001))
                      .status == CommandStatus::failed &&
              service.Snapshot().session->session,
          "Address verification requires disconnect and preserves the active "
          "session");
  Require(Complete(service.Disconnect()).status == CommandStatus::applied,
          "Disconnect before editing the saved endpoint");
  Require(Complete(service.EditReceiverEndpoint("AA:BB:CC:DD:EE:FF",
                                                "127.0.0.1", 7001))
                      .status == CommandStatus::applied &&
              service.Snapshot().receivers[0].manual_endpoint->port == 7001,
          "Edited endpoint is public-probed and retains its stable identity");
  Require(Complete(service.EditReceiverEndpoint("11:22:33:44:55:66",
                                                "127.0.0.1", 7001))
                      .status == CommandStatus::failed &&
              service.Snapshot().receivers[1].manual_endpoint->port == 7000,
          "Endpoint identifying a different receiver cannot inherit saved "
          "credentials");
  Require(Complete(service.Connect("AA:BB:CC:DD:EE:FF")).status ==
              CommandStatus::applied,
          "Verified edited endpoint is available for a new connection");
  Require(
      Complete(service.DeleteReceiver("AA:BB:CC:DD:EE:FF")).status ==
              CommandStatus::applied &&
          !service.Snapshot().session->session &&
          service.Snapshot().receivers.size() == 1,
      "Deleting active receiver stops capture and removes saved preferences");
  Require(Complete(service.UpdateReceiver("11:22:33:44:55:66", "Visible again",
                                          true, false, false, 120))
                  .status == CommandStatus::applied,
          "Hidden receiver can be restored in management");
  auto saved = SettingsStore(root).Load();
  Require(saved.receivers.size() == 1 && saved.receivers[0].favorite &&
              !saved.receivers[0].hidden && saved.receivers[0].volume == 0.4,
          "Persisted management and volume remain bound to receiver identity");
  ReceiverRegistry replay(saved.receivers);
  auto announced = saved.receivers.front();
  announced.name = "Updated public name";
  auto updated_endpoint = *announced.manual_endpoint;
  updated_endpoint.discovery_epoch = 7;
  updated_endpoint.resolved_address = "127.0.0.2";
  replay.Merge(announced, updated_endpoint);
  Require(replay.Find(announced.receiver_id).name == "Visible again" &&
              replay.Find(announced.receiver_id).volume == 0.4 &&
              replay.Find(announced.receiver_id).favorite,
          "Public name and address updates preserve edited preferences and "
          "identity");
  Require(Complete(service.Shutdown()).status == CommandStatus::applied &&
              receiver.created == receiver.destroyed,
          "Final shutdown owns no receiver runtime");
  {
    auto path = root / L"switch-cancel";
    SavedReceivers(path);
    VirtualReceiver peer;
    peer.hold_pairing = true;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator coordinator(peer, path, false,
                                  VirtualAudioServices(source));
    auto original = coordinator.Connect("AA:BB:CC:DD:EE:FF");
    WaitUntil(
        [&] {
          std::lock_guard lock(peer.mutex);
          return peer.paired == 1;
        },
        "Original pairing did not start");
    auto switched = coordinator.Connect("11:22:33:44:55:66");
    Require(Complete(std::move(original)).status == CommandStatus::canceled,
            "New selection cancels the incomplete original connection");
    WaitUntil(
        [&] {
          std::lock_guard lock(peer.mutex);
          return peer.paired == 2;
        },
        "New selected receiver did not own the connection attempt");
    {
      std::lock_guard lock(peer.mutex);
      peer.injected_state = std::pair<uint64_t, sender_state>{1, SENDER_READY};
    }
    WaitUntil(
        [&] {
          std::lock_guard lock(peer.mutex);
          return peer.late_states_delivered == 1;
        },
        "Late state callback was not delivered");
    Require(switched.wait_for(std::chrono::milliseconds(20)) !=
                    std::future_status::ready &&
                coordinator.Snapshot().session->session->state ==
                    SessionState::pairing &&
                coordinator.Snapshot().session->session->receiver_id ==
                    "11:22:33:44:55:66",
            "Old generation ready callback cannot complete the new connection");
    {
      std::lock_guard lock(peer.mutex);
      peer.hold_pairing = false;
      peer.reject_auth = true;
    }
    auto failure = Complete(std::move(switched));
    Require(failure.status == CommandStatus::failed &&
                failure.peer_code == 470 &&
                coordinator.Snapshot().session->intent.selected_receiver_id ==
                    "11:22:33:44:55:66",
            "New receiver failure preserves new selection and reports "
            "authentication error");
    Require(Complete(coordinator.Shutdown()).status == CommandStatus::applied &&
                peer.created == peer.destroyed,
            "Failed switch cleans both controlled runtimes");
  }
  {
    auto path = root / L"volume-cancel";
    SavedReceivers(path);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator coordinator(peer, path, false,
                                  VirtualAudioServices(source));
    Require(Complete(coordinator.Connect("AA:BB:CC:DD:EE:FF")).status ==
                CommandStatus::applied,
            "Connect before pending volume cancellation");
    {
      std::lock_guard lock(peer.mutex);
      peer.hold_volume = true;
      sender_command_completion late{};
      late.size = sizeof(late);
      late.version = 1;
      late.generation = 999;
      late.command_id = 1;
      late.volume_percent = 99;
      peer.injected_completion = late;
    }
    auto pending = coordinator.SetReceiverVolume("AA:BB:CC:DD:EE:FF", 0.9);
    WaitUntil([&] { return coordinator.Snapshot().volume_pending; },
              "Cancelable volume command did not begin");
    WaitUntil(
        [&] {
          std::lock_guard lock(peer.mutex);
          return peer.late_commands_delivered == 1;
        },
        "Old volume callback was not delivered");
    Require(pending.wait_for(std::chrono::milliseconds(20)) !=
                    std::future_status::ready &&
                coordinator.Snapshot().applied_volume == 50u,
            "Unrelated late completion cannot apply or resolve current volume "
            "command");
    auto disconnect = coordinator.Disconnect();
    Require(
        Complete(std::move(pending)).status == CommandStatus::canceled &&
            Complete(std::move(disconnect)).status == CommandStatus::applied &&
            coordinator.Snapshot().receivers[0].volume == 0.5,
        "Disconnect promptly cancels pending volume without persisting it");
    Require(Complete(coordinator.Shutdown()).status == CommandStatus::applied,
            "Canceled volume leaves no stranded promise");
  }
  {
    auto path = root / L"delete-failure";
    SavedReceivers(path);
    VirtualReceiver peer;
    peer.export_credentials = true;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator coordinator(peer, path, false,
                                  VirtualAudioServices(source));
    Require(Complete(coordinator.Connect("AA:BB:CC:DD:EE:FF")).status ==
                CommandStatus::applied,
            "Pair before credential deletion failure");
    Require(Complete(coordinator.DeleteReceiver("11:22:33:44:55:66")).status ==
                    CommandStatus::applied &&
                coordinator.Snapshot().session->session,
            "Deleting inactive receiver leaves active capture and callbacks "
            "intact");
    auto credential = path / L"credentials" / L"AA-BB-CC-DD-EE-FF.bin";
    Require(SetFileAttributesW(credential.c_str(), FILE_ATTRIBUTE_READONLY),
            "Inject credential deletion failure");
    Require(Complete(coordinator.DeleteReceiver("AA:BB:CC:DD:EE:FF")).status ==
                    CommandStatus::failed &&
                !coordinator.Snapshot().session->session &&
                coordinator.Snapshot().receivers.size() == 1,
            "Failed credential deletion stops playback but retains visible "
            "receiver record");
    Require(
        Complete(coordinator.Connect("AA:BB:CC:DD:EE:FF")).status ==
                CommandStatus::failed &&
            peer.imported == 0,
        "Revocation marker prevents reusing credential after deletion failure");
    Require(SetFileAttributesW(credential.c_str(), FILE_ATTRIBUTE_NORMAL),
            "Release credential deletion failure");
    Require(Complete(coordinator.DeleteReceiver("AA:BB:CC:DD:EE:FF")).status ==
                    CommandStatus::applied &&
                coordinator.Snapshot().receivers.empty(),
            "Deletion retry removes credential and receiver settings");
    Require(Complete(coordinator.Shutdown()).status == CommandStatus::applied &&
                peer.created == peer.destroyed,
            "Deletion failure recovery releases every owned runtime");
  }
  {
    auto path = root / L"tray-volume";
    SavedReceivers(path);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator coordinator(peer, path, false,
                                  VirtualAudioServices(source));
    Require(Complete(coordinator.Connect("AA:BB:CC:DD:EE:FF")).status ==
                CommandStatus::applied,
            "Connect a virtual receiver for tray volume interaction");
    TrayViewModel tray(coordinator);
    tray.Refresh();
    {
      std::lock_guard lock(peer.mutex);
      peer.hold_volume = true;
    }
    tray.Volume(0.6);
    WaitUntil([&] { return coordinator.Snapshot().volume_pending; },
              "Hold the first tray volume reply");
    for (unsigned value = 61; value <= 95; ++value) {
      tray.Volume(double(value) / 100);
      tray.Refresh();
      Require(tray.Snapshot().adjustable &&
                  tray.Snapshot().receiver_volume == double(value) / 100,
              "Tray dragging retains the latest thumb position during remote "
              "confirmation");
    }
    {
      std::lock_guard lock(peer.mutex);
      peer.hold_volume = false;
    }
    WaitUntil(
        [&] {
          tray.Refresh();
          return coordinator.Snapshot().applied_volume == 95u &&
                 !coordinator.Snapshot().volume_pending;
        },
        "Apply the latest coalesced tray volume");
    {
      std::lock_guard lock(peer.mutex);
      Require(peer.applied_volumes == std::vector<unsigned>({60, 95}),
              "Tray volume sends the initial and latest values rather than "
              "every intermediate position");
    }
    Require(Complete(coordinator.Shutdown()).status == CommandStatus::applied,
            "Tray volume interaction releases its virtual session");
  }
}
}  // namespace Win2Air::Tests
