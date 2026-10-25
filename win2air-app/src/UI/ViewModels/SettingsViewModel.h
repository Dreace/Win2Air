#pragma once
#include "../../Streaming/StreamCoordinator.h"
#include "SettingDraft.h"
namespace Win2Air {
class SettingsViewModel {
  StreamCoordinator& service_;
  std::optional<std::future<CommandResult>> pending_;
  PlaybackSnapshot snapshot_;
  CommandResult result_;
  std::shared_ptr<SettingsDraft> draft_;
  std::function<void()> applied_;
  std::function<void()> rejected_;
  void Submit(std::function<std::future<CommandResult>()> action,
              std::function<void()> applied = {},
              std::function<void()> rejected = {});

 public:
  explicit SettingsViewModel(
      StreamCoordinator& service,
      std::shared_ptr<SettingsDraft> draft = std::make_shared<SettingsDraft>())
      : service_(service), draft_(std::move(draft)) {}
  SettingsDraft& Draft() { return *draft_; }
  void Refresh();
  void Present(PlaybackSnapshot snapshot);
  PlaybackSnapshot const& Snapshot() const { return snapshot_; }
  CommandResult const& Result() const { return result_; }
  bool Pending() const { return pending_.has_value(); }
  void Appearance(std::string theme, std::string language);
  void Source(AudioSourceConfig source);
  void Local(bool enabled);
  void Logging(std::string level) {
    Submit([this, level = std::move(level)] {
      return service_.SetLogLevel(level);
    });
  }
  void ReceiverMediaControl(bool enabled) {
    Submit(
        [this, enabled] { return service_.SetReceiverMediaControl(enabled); });
  }
  void Lifecycle(bool close_to_tray, bool minimized, bool restore_previous);
};
}  // namespace Win2Air
