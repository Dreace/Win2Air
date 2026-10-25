#pragma once
#include <algorithm>
#include <stdexcept>
#include "../Settings/AppSettings.h"
#include "SessionModels.h"
namespace Win2Air {
inline constexpr int kRenderLeadMs = 8;
inline constexpr int kSendSafetyMs = 2;
inline constexpr int kAdaptiveOriginMs = 120;
inline constexpr int kAdaptiveStepMs = 10;
inline int AdaptiveRungAtLeast(int milliseconds) {
  if (milliseconds >= kAdaptiveOriginMs) {
    auto delta = milliseconds - kAdaptiveOriginMs;
    return kAdaptiveOriginMs +
           (delta + kAdaptiveStepMs - 1) / kAdaptiveStepMs * kAdaptiveStepMs;
  }
  auto room = kAdaptiveOriginMs - milliseconds;
  return kAdaptiveOriginMs - room / kAdaptiveStepMs * kAdaptiveStepMs;
}
inline int AdaptiveRungBelow(int applied) {
  auto rung = AdaptiveRungAtLeast(applied);
  return (rung == applied ? applied : rung) - kAdaptiveStepMs;
}
PresentationConfig InitialPresentation(AppSettings const&,
                                       ReceiverRecord const&);
inline PresentationConfig ReceiverPresentation(
    PresentationConfig config, sender_timing_info const& timing) {
  if (config.mode != "adaptive") return config;
  if (!timing.receiver_render_available)
    throw std::invalid_argument(
        "Adaptive latency requires arrivalToRenderLatencyMs from the receiver");
  constexpr uint64_t kReceiverSafetyMs = 25;
  auto rounded =
      (uint64_t(timing.receiver_render_ms) + kReceiverSafetyMs + 9) / 10 * 10;
  if (rounded < SENDER_MIN_PRESENTATION_MS ||
      rounded > SENDER_MAX_PRESENTATION_MS ||
      int64_t(rounded) + config.offset_ms < SENDER_MIN_PRESENTATION_MS ||
      int64_t(rounded) + config.offset_ms > SENDER_MAX_PRESENTATION_MS)
    throw std::invalid_argument(
        "Receiver arrivalToRenderLatencyMs is outside the presentation "
        "range: " +
        std::to_string(timing.receiver_render_ms));
  config.base_ms = int(rounded);
  config.effective_ms = config.base_ms + config.offset_ms;
  return config;
}
inline bool TightPlaybackBudget(PresentationConfig const& config) {
  return config.mode == "ultra" || config.mode == "low" ||
         config.mode == "adaptive" || config.effective_ms < 100;
}
inline unsigned CaptureBufferMs(PresentationConfig const& config) {
  if (TightPlaybackBudget(config))
    // Capacity absorbs capture-thread scheduling bursts; packets are consumed
    // immediately. A fixed tiny ring discards valid audio before its deadline.
    return unsigned(std::clamp(
        config.effective_ms - kRenderLeadMs - kSendSafetyMs, 10, 250));
  return unsigned(std::clamp(config.effective_ms / 5, 100, 250));
}
inline size_t PcmCapacity(PresentationConfig const& config) {
  return TightPlaybackBudget(config) ? 1764 : 44100;
}
inline size_t SenderAudioCapacity(PresentationConfig const& config) {
  if (TightPlaybackBudget(config)) return 16u * 44100u / 1000u * 4u;
  return static_cast<size_t>(config.effective_ms + 250) * 44100u / 1000u * 4u;
}
}  // namespace Win2Air
