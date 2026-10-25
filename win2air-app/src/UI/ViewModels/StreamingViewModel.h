#pragma once
#include "../../Streaming/StreamCoordinator.h"
#include "SettingDraft.h"
namespace Win2Air {
struct StreamingSnapshot {
  PlaybackSnapshot playback;
  std::string receiver_id, error;
  int offset_ms{};
  bool pending{};
};
class StreamingViewModel {
  StreamCoordinator& service_;
  struct PendingCommand {
    std::future<CommandResult> result;
    std::function<void()> applied;
    std::function<void()> rejected;
  };
  std::vector<PendingCommand> pending_;
  StreamingSnapshot snapshot_;
  std::shared_ptr<StreamingDraft> draft_;

 public:
  explicit StreamingViewModel(StreamCoordinator& service,
                              std::shared_ptr<StreamingDraft> draft =
                                  std::make_shared<StreamingDraft>())
      : service_(service), draft_(std::move(draft)) {}
  StreamingDraft& Draft() { return *draft_; }
  void Refresh();
  StreamingSnapshot const& Snapshot() const { return snapshot_; }
  std::pair<int, int> OffsetRange() const;
  void Latency(std::string mode, int custom_ms);
  void Offset(int value);
  void Silence(int seconds);
  void StandbyDisconnect(int seconds);
  void AutoReconnect(bool enabled);
};
}  // namespace Win2Air
