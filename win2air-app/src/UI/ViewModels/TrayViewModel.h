#pragma once
#include "../../Streaming/StreamCoordinator.h"
#include "VolumeInput.h"
namespace Win2Air {
struct TraySnapshot {
  std::string receiver_id, name, model, detail, error;
  SessionState state{SessionState::disconnected};
  StandbyState standby{StandbyState::active};
  double receiver_volume{0.5};
  bool muted{}, adjustable{}, authentication{};
  std::optional<unsigned> retry_seconds;
};
TraySnapshot ProjectTray(PlaybackSnapshot const& snapshot);
class TrayViewModel {
  StreamCoordinator& service_;
  std::vector<std::future<CommandResult>> pending_;
  TraySnapshot snapshot_;
  VolumeInput volume_;
  std::string volume_receiver_;

 public:
  explicit TrayViewModel(StreamCoordinator& service) : service_(service) {}
  void Refresh();
  TraySnapshot const& Snapshot() const { return snapshot_; }
  void Volume(double value);
  void Mute(bool value);
};
}  // namespace Win2Air
