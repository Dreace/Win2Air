#pragma once
#include <cstdint>
namespace Win2Air {
enum class ClockAction { stable, correct_anchor, reanchor };
enum class CaptureReadiness { ready, wait, expired };
CaptureReadiness CaptureTime(uint64_t capture_ns, uint64_t now_ns,
                             uint64_t budget_ns);
class AudioClock {
  uint64_t epoch_{}, anchor_{}, first_drift_{};

 public:
  void Begin(uint64_t epoch, uint64_t capture_ns);
  uint64_t Stamp(uint64_t sample) const;
  ClockAction Observe(uint64_t sample, uint64_t actual_ns, uint64_t now_ns);
};
uint64_t MonotonicNs();
}  // namespace Win2Air
