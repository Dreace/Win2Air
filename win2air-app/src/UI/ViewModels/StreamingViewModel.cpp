#include "StreamingViewModel.h"
#include "../../Streaming/PresentationConfig.h"
namespace Win2Air {
void StreamingViewModel::Refresh() {
  for (auto item = pending_.begin(); item != pending_.end();) {
    if (item->result.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      ++item;
      continue;
    }
    auto result = item->result.get();
    if (result.status == CommandStatus::failed ||
        result.status == CommandStatus::rejected)
      snapshot_.error = result.message;
    if (result.status == CommandStatus::applied) {
      snapshot_.error.clear();
      if (item->applied) item->applied();
    } else if (item->rejected)
      item->rejected();
    item = pending_.erase(item);
  }
  snapshot_.playback = service_.Snapshot();
  snapshot_.pending =
      !pending_.empty() || snapshot_.playback.presentation_pending;
  snapshot_.receiver_id.clear();
  snapshot_.offset_ms = 0;
  if (snapshot_.playback.session && snapshot_.playback.session->session) {
    snapshot_.receiver_id = snapshot_.playback.session->session->receiver_id;
    for (auto const& record : snapshot_.playback.receivers)
      if (record.receiver_id == snapshot_.receiver_id)
        snapshot_.offset_ms = record.offset_ms;
  }
  auto const& saved = snapshot_.playback.preferences;
  draft_->mode.Update(saved.latency_mode);
  draft_->custom.Update(saved.custom_ms);
  if (draft_->receiver != snapshot_.receiver_id) {
    draft_->offset = {};
    draft_->receiver = snapshot_.receiver_id;
  }
  draft_->offset.Update(snapshot_.offset_ms);
}
void StreamingViewModel::Latency(std::string mode, int custom_ms) {
  auto rejected = [draft = draft_,
                   saved = snapshot_.playback.preferences.latency_mode,
                   version = draft_->mode.Version()] {
    draft->mode.Reject(version, saved);
  };
  auto candidate = snapshot_.playback.preferences;
  candidate.latency_mode = mode;
  candidate.custom_ms = custom_ms;
  ReceiverRecord receiver;
  receiver.offset_ms = snapshot_.offset_ms;
  InitialPresentation(candidate, receiver);
  auto applied = [draft = draft_, mode, custom_ms, mv = draft_->mode.Version(),
                  cv = draft_->custom.Version()] {
    draft->mode.Acknowledge(mv, mode);
    if (mode == "custom") draft->custom.Acknowledge(cv, custom_ms);
  };
  pending_.push_back({service_.SetLatency(std::move(mode), custom_ms),
                      std::move(applied), std::move(rejected)});
}
std::pair<int, int> StreamingViewModel::OffsetRange() const {
  auto base = InitialPresentation(snapshot_.playback.preferences, {}).base_ms;
  if (snapshot_.playback.preferences.latency_mode == "adaptive" &&
      snapshot_.playback.timing.setup_applied)
    base = snapshot_.playback.timing.base_ms;
  return {std::max(-1000, SENDER_MIN_PRESENTATION_MS - base),
          std::min(1000, SENDER_MAX_PRESENTATION_MS - base)};
}
void StreamingViewModel::Offset(int value) {
  auto [minimum, maximum] = OffsetRange();
  if (value < minimum || value > maximum)
    throw std::invalid_argument(
        "Offset exceeds the effective presentation budget range");
  pending_.push_back({service_.SetOffset(snapshot_.receiver_id, value),
                      [draft = draft_, value, receiver = snapshot_.receiver_id,
                       version = draft_->offset.Version()] {
                        if (draft->receiver == receiver)
                          draft->offset.Acknowledge(version, value);
                      }});
}
void StreamingViewModel::Silence(int seconds) {
  pending_.push_back({service_.SetSilenceTimeout(seconds), {}});
}
void StreamingViewModel::AutoReconnect(bool enabled) {
  pending_.push_back({service_.SetAutoReconnect(enabled), {}});
}
void StreamingViewModel::StandbyDisconnect(int seconds) {
  pending_.push_back({service_.SetStandbyDisconnect(seconds), {}});
}
}  // namespace Win2Air
