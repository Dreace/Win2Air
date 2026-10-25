#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include "PresentationConfig.h"

namespace Win2Air {
struct LatencyObservation {
  uint64_t now_ns{}, generation{}, sent{}, retransmitted{}, discontinuities{};
  double queue_age_ms{};
  bool interrupted{};
};
struct LatencyProposal {
  int milliseconds{};
  char const* reason{"lower"};
};
class AdaptiveLatencyController {
  static constexpr uint64_t kResendWindowNs = 10000000000ULL;
  static constexpr uint64_t kStableNs = 30000000000ULL;
  int applied_{120}, origin_{120}, offset_{};
  int maximum_{SENDER_MAX_PRESENTATION_MS};
  uint64_t stable_since_{}, retry_after_{}, lowered_at_{};
  bool recovery_locked_{}, lock_on_raise_{};
  std::optional<LatencyObservation> previous_;
  std::deque<uint64_t> requests_;

 public:
  int Applied() const noexcept { return applied_; }
  int Offset() const noexcept { return offset_; }
  bool RecoveryLocked() const noexcept { return recovery_locked_; }
  uint64_t StableFor(uint64_t now) const noexcept {
    return previous_ && stable_since_ && now >= stable_since_
               ? now - stable_since_
               : 0;
  }
  void Reset(int applied, int offset = 0) {
    if (applied < SENDER_MIN_PRESENTATION_MS ||
        applied > SENDER_MAX_PRESENTATION_MS || offset < -1000 ||
        offset > 1000 || applied + offset < SENDER_MIN_PRESENTATION_MS ||
        applied + offset > SENDER_MAX_PRESENTATION_MS)
      throw std::invalid_argument("Adaptive presentation budget is invalid");
    applied_ = origin_ = applied;
    offset_ = offset;
    maximum_ = std::min(SENDER_MAX_PRESENTATION_MS,
                        SENDER_MAX_PRESENTATION_MS - offset);
    stable_since_ = retry_after_ = lowered_at_ = 0;
    recovery_locked_ = lock_on_raise_ = false;
    previous_.reset();
    requests_.clear();
  }
  void Defer(uint64_t) noexcept {
    stable_since_ = 0;
    previous_.reset();
    requests_.clear();
  }
  void Confirm(int applied, uint64_t now) {
    if (!now || applied < origin_ || applied > maximum_)
      throw std::invalid_argument(
          "Adaptive confirmation is outside the session range");
    if (applied < applied_) lowered_at_ = now;
    if (applied > applied_ && lock_on_raise_) recovery_locked_ = true;
    applied_ = applied;
    stable_since_ = now;
    lock_on_raise_ = false;
    requests_.clear();
  }
  void Reject(uint64_t now) noexcept {
    stable_since_ = 0;
    retry_after_ = now + kStableNs;
    lock_on_raise_ = false;
  }
  std::optional<LatencyProposal> Observe(LatencyObservation value) {
    if (!value.now_ns || !value.generation ||
        !std::isfinite(value.queue_age_ms) || value.queue_age_ms < 0)
      throw std::invalid_argument("Adaptive observation lacks a valid clock");
    if (previous_ && previous_->generation != value.generation)
      Reset(applied_, offset_);
    if (!previous_ || value.now_ns <= previous_->now_ns ||
        value.now_ns - previous_->now_ns > 5000000000ULL ||
        value.sent < previous_->sent ||
        value.retransmitted < previous_->retransmitted ||
        value.discontinuities < previous_->discontinuities) {
      previous_ = value;
      stable_since_ = value.now_ns;
      requests_.clear();
      return {};
    }
    auto sent = value.sent - previous_->sent;
    auto resends = value.retransmitted - previous_->retransmitted;
    bool interrupted = value.interrupted ||
                       value.discontinuities != previous_->discontinuities;
    previous_ = value;
    while (!requests_.empty() &&
           value.now_ns - requests_.front() >= kResendWindowNs)
      requests_.pop_front();
    for (uint64_t index = 0; index < std::min(resends, uint64_t{3}); ++index)
      requests_.push_back(value.now_ns);
    while (requests_.size() > 3) requests_.pop_front();
    if (!stable_since_ || resends || !sent || interrupted)
      stable_since_ = value.now_ns;
    if (value.now_ns < retry_after_) return {};
    if (requests_.size() >= 3 && applied_ < maximum_) {
      lock_on_raise_ = lowered_at_ && value.now_ns >= lowered_at_ &&
                       value.now_ns - lowered_at_ <= kResendWindowNs;
      return LatencyProposal{
          std::min(maximum_, AdaptiveRungAtLeast(applied_ + 1)), "resend"};
    }
    if (!recovery_locked_ && sent && !resends && !interrupted &&
        value.now_ns - stable_since_ >= kStableNs && applied_ > origin_) {
      auto next = std::max(origin_, AdaptiveRungBelow(applied_));
      // Keep enough room for PCM already waiting on the local send path.
      if (value.queue_age_ms + kRenderLeadMs + kSendSafetyMs < next + offset_)
        return LatencyProposal{next, "lower"};
    }
    return {};
  }
};
}  // namespace Win2Air
