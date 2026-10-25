#include "TrayViewModel.h"
#include "ReceiverViewModel.h"
namespace Win2Air {
TraySnapshot ProjectTray(PlaybackSnapshot const& snapshot) {
  TraySnapshot result;
  if (!snapshot.session || !snapshot.session->session) return result;
  auto const& active = *snapshot.session->session;
  result.receiver_id = active.receiver_id;
  result.state = active.state;
  result.standby = active.standby_state;
  result.authentication = active.failure_kind == "authentication";
  result.adjustable = !snapshot.presentation_pending &&
                      (active.state == SessionState::ready ||
                       active.state == SessionState::streaming);
  for (auto const& receiver : snapshot.receivers)
    if (receiver.receiver_id == active.receiver_id) {
      result.name = receiver.name;
      result.detail = ReceiverDetail(receiver);
      result.model =
          ProductName(receiver.model, resource_language.starts_with("zh"),
                      receiver.manufacturer);
      result.receiver_volume = receiver.volume;
      result.muted = receiver.muted;
    }
  if (active.error) result.error = active.error->message;
  for (auto const& view : ReceiverViews(snapshot, true))
    if (view.receiver.receiver_id == active.receiver_id) {
      result.retry_seconds = view.retry_seconds;
      result.error = view.recovery_reason;
    }
  return result;
}
void TrayViewModel::Refresh() {
  std::string error;
  bool completed{};
  if (auto result = volume_.Complete()) {
    completed = true;
    if (result->status == CommandStatus::failed ||
        result->status == CommandStatus::rejected)
      error = result->message;
  }
  for (auto item = pending_.begin(); item != pending_.end();) {
    if (item->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++item;
      continue;
    }
    auto result = item->get();
    completed = true;
    if (result.status == CommandStatus::failed ||
        result.status == CommandStatus::rejected)
      error = result.message;
    item = pending_.erase(item);
  }
  auto previous_error = snapshot_.error;
  snapshot_ = ProjectTray(service_.Snapshot());
  if (volume_receiver_ != snapshot_.receiver_id) {
    volume_ = {};
    volume_receiver_ = snapshot_.receiver_id;
  }
  volume_.Update(snapshot_.receiver_volume);
  snapshot_.receiver_volume = volume_.Value();
  volume_.Send([this](double value) {
    return service_.SetReceiverVolume(volume_receiver_, value);
  });
  if (!error.empty())
    snapshot_.error = error;
  else if (!completed && snapshot_.error.empty())
    snapshot_.error = previous_error;
}
void TrayViewModel::Volume(double value) {
  volume_.Edit(value);
  snapshot_.receiver_volume = volume_.Value();
  volume_.Send([this](double next) {
    return service_.SetReceiverVolume(volume_receiver_, next);
  });
}
void TrayViewModel::Mute(bool value) {
  pending_.push_back(service_.SetReceiverMuted(snapshot_.receiver_id, value));
}
}  // namespace Win2Air
