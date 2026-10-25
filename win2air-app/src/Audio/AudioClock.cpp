#include "AudioClock.h"

#include <windows.h>

#include <stdexcept>
namespace Win2Air {
CaptureReadiness CaptureTime(uint64_t capture_ns, uint64_t now_ns,
                             uint64_t budget_ns) {
  if (!capture_ns || !now_ns || !budget_ns)
    throw std::runtime_error(
        "Capture scheduling requires a valid timestamp, clock and budget");
  if (capture_ns > now_ns && capture_ns - now_ns > 5000000)
    return CaptureReadiness::wait;
  if (now_ns > capture_ns && now_ns - capture_ns > budget_ns)
    return CaptureReadiness::expired;
  return CaptureReadiness::ready;
}
uint64_t MonotonicNs() {
  LARGE_INTEGER counter{}, frequency{};
  if (!QueryPerformanceCounter(&counter) ||
      !QueryPerformanceFrequency(&frequency) || counter.QuadPart < 0 ||
      frequency.QuadPart <= 0)
    throw std::runtime_error("Read monotonic audio clock failed");
  return uint64_t(counter.QuadPart / frequency.QuadPart) * 1000000000 +
         uint64_t(counter.QuadPart % frequency.QuadPart) * 1000000000 /
             uint64_t(frequency.QuadPart);
}
void AudioClock::Begin(uint64_t epoch, uint64_t capture_ns) {
  if (!epoch || !capture_ns)
    throw std::runtime_error(
        "Capture clock requires an explicit source epoch and anchor");
  epoch_ = epoch;
  anchor_ = capture_ns;
  first_drift_ = 0;
}
uint64_t AudioClock::Stamp(uint64_t sample) const {
  if (!epoch_ || sample / 44100 > (UINT64_MAX - anchor_) / 1000000000)
    throw std::runtime_error("Capture sample clock is absent or overflows");
  auto whole = anchor_ + sample / 44100 * 1000000000;
  auto fraction = sample % 44100 * 1000000000 / 44100;
  if (fraction > UINT64_MAX - whole)
    throw std::runtime_error("Capture sample fraction overflows");
  return whole + fraction;
}
ClockAction AudioClock::Observe(uint64_t sample, uint64_t actual_ns,
                                uint64_t now_ns) {
  auto expected = Stamp(sample);
  auto drift =
      expected > actual_ns ? expected - actual_ns : actual_ns - expected;
  if (drift > 20000000) {
    first_drift_ = 0;
    return ClockAction::reanchor;
  }
  if (drift > 5000000) {
    if (!first_drift_) first_drift_ = now_ns;
    if (now_ns >= first_drift_ && now_ns - first_drift_ >= 3000000000) {
      auto elapsed =
          sample / 44100 * 1000000000 + sample % 44100 * 1000000000 / 44100;
      if (actual_ns < elapsed)
        throw std::runtime_error("Source clock correction underflows");
      anchor_ = actual_ns - elapsed;
      first_drift_ = 0;
      return ClockAction::correct_anchor;
    }
  } else
    first_drift_ = 0;
  return ClockAction::stable;
}
}  // namespace Win2Air
