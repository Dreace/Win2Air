#include "receiver_controls_support.h"

namespace Win2Air::Tests {

void SystemMediaScenario(std::filesystem::path const& root) {
  auto media_root = root / "system-media";
  auto initial = SavedReceivers(media_root);
  auto source = std::make_shared<VirtualAudioState>();
  VirtualReceiver peer;
  std::atomic<uint64_t> sample_time{1000000000};
  std::atomic<std::shared_ptr<NowPlaying const>> item;
  auto playing = std::make_shared<NowPlaying>();
  playing->revision = 1;
  playing->title = "Current title";
  playing->artist = "Current artist";
  playing->album = "Current album";
  playing->artwork_type = "image/jpeg";
  playing->artwork = {0xff, 0xd8, 0xff, 0xd9};
  item = playing;
  auto hooks = VirtualAudioServices(source);
  hooks.metrics_now = [&] { return sample_time.load(); };
  hooks.now_playing = [&] { return item.load(); };
  StreamCoordinator coordinator(peer, media_root, false, hooks);
  Require(Complete(coordinator.Connect(initial.receivers.front().receiver_id))
                  .status == CommandStatus::applied,
          "Connect virtual receiver for system media and reverse volume");
  sample_time += 1000000000;
  WaitUntil(
      [&] {
        std::lock_guard lock(peer.mutex);
        return peer.metadata_requests == 1;
      },
      "Current system media metadata must reach the receiver");
  size_t sent_volumes;
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.media_title == playing->title &&
                peer.media_artist == playing->artist &&
                peer.media_album == playing->album &&
                peer.media_artwork == playing->artwork,
            "Title, album and artwork retain their exact system values");
    peer.observed_volume = 37;
    peer.observed_volume_revision = 1;
    sent_volumes = peer.applied_volumes.size();
  }
  sample_time += 1000000000;
  WaitUntil([&] { return coordinator.Snapshot().applied_volume == 37u; },
            "Receiver volume changes must reach the UI snapshot");
  auto snapshot = coordinator.Snapshot();
  Require(std::abs(snapshot.receivers.front().volume - 0.37) < 0.001,
          "Receiver observations update the same volume shown by playback "
          "and tray");
  Require(std::abs(ProjectTray(snapshot).receiver_volume - 0.37) < 0.001,
          "Tray projection shows the receiver-reported volume");
  sender_command_completion metadata_reply{};
  metadata_reply.size = sizeof(metadata_reply);
  metadata_reply.version = 1;
  metadata_reply.generation = snapshot.session->session->generation;
  metadata_reply.kind = SENDER_COMMAND_METADATA;
  metadata_reply.result = SENDER_NETWORK;
  strcpy_s(metadata_reply.error.message,
           "Controlled receiver metadata rejection");
  {
    std::lock_guard lock(peer.mutex);
    peer.injected_completion = metadata_reply;
  }
  WaitUntil(
      [&] {
        return coordinator.Snapshot().metadata_error ==
               metadata_reply.error.message;
      },
      "Asynchronous metadata rejection must reach the UI");
  Require(coordinator.Snapshot().session->session->state == SessionState::ready,
          "Metadata rejection preserves the audio session");
  metadata_reply.result = SENDER_OK;
  {
    std::lock_guard lock(peer.mutex);
    peer.injected_completion = metadata_reply;
  }
  WaitUntil(
      [&] { return coordinator.Snapshot().metadata_error.empty(); },
      "A successful metadata completion clears the previous transport error");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.applied_volumes.size() == sent_volumes,
            "Receiver volume observation must not echo a volume command");
    peer.metadata_backpressure = true;
  }
  auto cleared = std::make_shared<NowPlaying>();
  cleared->revision = 2;
  item = cleared;
  sample_time += 1000000000;
  WaitUntil(
      [&] { return coordinator.Snapshot().metrics.sample_time == sample_time; },
      "A busy control channel keeps current metadata pending");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.metadata_requests == 1,
            "Backpressure preserves the latest pending media");
    peer.metadata_backpressure = false;
  }
  sample_time += 1000000000;
  WaitUntil(
      [&] {
        std::lock_guard lock(peer.mutex);
        return peer.metadata_requests == 2;
      },
      "Cleared system media retries after control channel backpressure");
  {
    std::lock_guard lock(peer.mutex);
    Require(peer.media_title.empty() && peer.media_album.empty() &&
                peer.media_artwork.empty(),
            "Ending the current system session clears stale receiver metadata");
  }
  Require(Complete(coordinator.Disconnect()).status == CommandStatus::applied,
          "Disconnect the virtual media session");
  Require(Complete(coordinator.Connect(initial.receivers.front().receiver_id))
                  .status == CommandStatus::applied,
          "Reconnect the virtual media session");
  sample_time += 1000000000;
  WaitUntil(
      [&] {
        std::lock_guard lock(peer.mutex);
        return peer.metadata_requests == 3;
      },
      "A new receiver generation receives the current metadata again");
  Require(Complete(coordinator.Shutdown()).status == CommandStatus::applied,
          "Release the virtual media receiver");
}
void VolumeInputScenario() {
  VolumeInput input;
  input.Update(0.5);
  std::vector<double> sent;
  std::vector<std::promise<CommandResult>> replies;
  auto send = [&](double value) {
    sent.push_back(value);
    replies.emplace_back();
    return replies.back().get_future();
  };
  input.Edit(0.6);
  input.Send(send);
  for (unsigned value = 61; value <= 95; ++value) {
    input.Edit(double(value) / 100);
    input.Update(0.5);
    input.Send(send);
  }
  Require(input.Value() == 0.95 && sent == std::vector<double>{0.6},
          "Dragging retains the latest position and coalesces pending input");
  CommandResult applied;
  applied.status = CommandStatus::applied;
  replies.front().set_value(applied);
  Require(input.Complete().has_value(), "The old volume reply completes");
  input.Update(0.6);
  input.Send(send);
  Require(input.Value() == 0.95 && sent == std::vector<double>({0.6, 0.95}),
          "An old reply never moves the thumb back and sends the latest value");
  replies.back().set_value(applied);
  input.Complete();
  input.Update(0.95);
  input.Send(send);
  Require(sent.size() == 2, "Confirmed input does not resend indefinitely");
  input.Edit(0.2);
  input.Send(send);
  CommandResult rejected;
  rejected.status = CommandStatus::rejected;
  replies.back().set_value(rejected);
  input.Complete();
  input.Update(0.95);
  input.Send(send);
  Require(input.Value() == 0.95 && sent.size() == 3,
          "Rejected latest input restores confirmed volume without retrying");
  Rejects([&] { input.Edit(1.1); }, "Volume input rejects invalid values");
}
}  // namespace Win2Air::Tests
