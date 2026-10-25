#include "VolumeController.h"

#include <cmath>
#include <stdexcept>
namespace Win2Air {
unsigned EffectiveVolume(ReceiverRecord const& receiver) {
  if (!std::isfinite(receiver.volume) || receiver.volume < 0 ||
      receiver.volume > 1)
    throw std::runtime_error("Volume must be finite and within zero to one");
  return receiver.muted
             ? 0u
             : static_cast<unsigned>(std::lround(receiver.volume * 100));
}
}  // namespace Win2Air
