#include "SilenceDetector.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace Win2Air {
void SilenceDetector::Configure(int seconds, uint64_t now) {
  if (seconds != 0 && seconds != 10 && seconds != 30 && seconds != 60)
    throw std::runtime_error(
        "Silence timeout must be Never, 10, 30 or 60 seconds");
  timeout_ = seconds;
  Discontinuity(now);
}
void SilenceDetector::Discontinuity(uint64_t now) {
  since_ = observed_ = now;
  frames_ = voiced_ = 0;
  energy_[0] = energy_[1] = 0;
  continuous_ = false;
  silent_ = true;
}
bool SilenceDetector::Consume(ConvertedBlock const& block, uint64_t now) {
  if (!block.source_epoch || !block.frame_count ||
      block.pcm.size() != size_t(block.frame_count) * 4 ||
      block.first_sample > UINT64_MAX - block.frame_count)
    throw std::runtime_error(
        "Silence detection requires complete stereo PCM frames");
  if (continuous_ &&
      (source_ != block.source_epoch || next_ != block.first_sample))
    Discontinuity(now);
  source_ = block.source_epoch;
  next_ = block.first_sample + block.frame_count;
  continuous_ = true;
  observed_ = now;
  bool wake{};
  const double voice = std::pow(10.0, -75.0 / 10) * 32768.0 * 32768.0;
  const double silence = std::pow(10.0, -80.0 / 10) * 32768.0 * 32768.0;
  for (size_t offset = 0; offset < block.pcm.size(); offset += 4) {
    for (unsigned channel = 0; channel < 2; ++channel) {
      int16_t sample;
      std::memcpy(&sample, block.pcm.data() + offset + channel * 2, 2);
      energy_[channel] += double(sample) * sample;
    }
    if (++frames_ != 441) continue;
    auto rms_squared = std::max(energy_[0], energy_[1]) / frames_;
    silent_ = rms_squared < silence;
    if (!silent_) since_ = now;
    voiced_ = rms_squared > voice ? std::min(voiced_ + 1, 2u) : 0;
    wake = wake || voiced_ == 2;
    frames_ = 0;
    energy_[0] = energy_[1] = 0;
  }
  return wake;
}
bool SilenceDetector::Due(uint64_t now) const {
  return timeout_ && now >= since_ &&
         now - since_ >= uint64_t(timeout_) * 1000000000;
}
bool SilenceDetector::Pending(uint64_t now) const {
  return timeout_ &&
         (silent_ || (now >= observed_ && now - observed_ >= 100000000));
}
}  // namespace Win2Air
