#include "ReceiverViewModel.h"

#include <algorithm>
namespace Win2Air {
std::vector<ReceiverViewModel> ReceiverViews(PlaybackSnapshot const& snapshot,
                                             bool include_hidden) {
  std::vector<ReceiverViewModel> views;
  for (auto const& record : snapshot.receivers) {
    if (record.hidden && !include_hidden) continue;
    ReceiverViewModel view;
    view.receiver = record;
    if (snapshot.session && snapshot.session->session &&
        snapshot.session->session->receiver_id == record.receiver_id) {
      view.active = snapshot.session->intent.desired_connected;
      view.state = snapshot.session->session->state;
      view.pending = snapshot.volume_pending || snapshot.presentation_pending;
      view.applied_volume = snapshot.applied_volume;
      if (snapshot.metrics.receiver_id == record.receiver_id)
        view.health = snapshot.health.level;
      auto const& session = *snapshot.session->session;
      view.failure_kind = session.failure_kind;
      if (session.error) view.recovery_reason = session.error->message;
      if (snapshot.reconnect) {
        view.recovery_reason = snapshot.reconnect->last_reason;
        auto due = snapshot.reconnect->next_due_monotonic;
        view.retry_seconds = static_cast<unsigned>(
            due > snapshot.recovery_now
                ? (due - snapshot.recovery_now + 999999999) / 1000000000
                : 0);
      }
    }
    view.adjustable = !snapshot.presentation_pending &&
                      (!view.active || view.state == SessionState::ready ||
                       view.state == SessionState::streaming);
    views.push_back(std::move(view));
  }
  std::stable_sort(views.begin(), views.end(),
                   [](auto const& left, auto const& right) {
                     return left.receiver.favorite > right.receiver.favorite;
                   });
  return views;
}
std::optional<int> ReceiverSelection::Project(
    PlaybackSnapshot const& snapshot,
    std::vector<ReceiverViewModel> const& visible) {
  if (!snapshot.session) return std::nullopt;
  auto const& intent = snapshot.session->intent.selected_receiver_id;
  bool connected = snapshot.session->intent.desired_connected;
  bool changed =
      intent != projected_intent_ || (connected && !projected_connected_);
  projected_intent_ = intent;
  projected_connected_ = connected;
  if (!changed) return std::nullopt;
  auto found = std::find_if(
      visible.begin(), visible.end(),
      [&](auto const& view) { return view.receiver.receiver_id == intent; });
  return found == visible.end() ? -1
                                : static_cast<int>(found - visible.begin());
}
}  // namespace Win2Air
