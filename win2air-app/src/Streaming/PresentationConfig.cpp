#include "PresentationConfig.h"

#include <stdexcept>
namespace Win2Air {
PresentationConfig InitialPresentation(AppSettings const &settings,
                                       ReceiverRecord const &receiver) {
  PresentationConfig config;
  config.mode = settings.latency_mode;
  if (config.mode == "low" || config.mode == "adaptive")
    config.base_ms = 120;
  else if (config.mode == "ultra")
    config.base_ms = 80;
  else if (config.mode == "balanced")
    config.base_ms = 1000;
  else if (config.mode == "stable")
    config.base_ms = 2000;
  else if (config.mode == "custom")
    config.base_ms = settings.custom_ms;
  else
    throw std::runtime_error("Unknown presentation mode");
  config.offset_ms = receiver.offset_ms;
  config.effective_ms = config.base_ms + config.offset_ms;
  if (config.base_ms < SENDER_MIN_PRESENTATION_MS ||
      config.base_ms > SENDER_MAX_PRESENTATION_MS || config.offset_ms < -1000 ||
      config.offset_ms > 1000 ||
      config.effective_ms < SENDER_MIN_PRESENTATION_MS ||
      config.effective_ms > SENDER_MAX_PRESENTATION_MS)
    throw std::runtime_error(
        "Initial presentation base, offset or effective budget is outside its "
        "range");
  return config;
}
}  // namespace Win2Air
