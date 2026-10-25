#include "ReconnectController.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace Win2Air {
void ReconnectController::Schedule(uint64_t now, uint64_t epoch,
                                   std::string reason) {
  auto attempt = state_.attempt;
  if (attempt < std::numeric_limits<unsigned>::max()) ++attempt;
  Arm(now, epoch, std::move(reason), attempt);
}
void ReconnectController::Resume(uint64_t now, uint64_t epoch,
                                 std::string reason) {
  Arm(now, epoch, std::move(reason), std::max(1u, state_.attempt));
}
void ReconnectController::Arm(uint64_t now, uint64_t epoch, std::string reason,
                              unsigned attempt) {
  if (!epoch || reason.empty())
    throw std::invalid_argument(
        "Reconnect requires an intent and failure reason");
  constexpr unsigned delays[] = {2, 5, 10, 30, 60};
  auto delay = uint64_t(delays[std::min(attempt - 1, 4u)]) * 1000000000;
  if (now > UINT64_MAX - delay)
    throw std::overflow_error("Reconnect deadline exceeds monotonic range");
  state_.attempt = attempt;
  state_.next_due_monotonic = now + delay;
  state_.cancellation_epoch = epoch;
  state_.last_reason = std::move(reason);
  waiting_ = true;
}
bool ReconnectController::Due(uint64_t now, uint64_t epoch) const noexcept {
  return waiting_ && epoch == state_.cancellation_epoch &&
         now >= state_.next_due_monotonic;
}
void ReconnectController::Cancel() noexcept { waiting_ = false; }
void ReconnectController::Reset() noexcept {
  waiting_ = false;
  state_ = {};
}
std::optional<ReconnectState> ReconnectController::Snapshot() const {
  return waiting_ ? std::optional(state_) : std::nullopt;
}
}  // namespace Win2Air
