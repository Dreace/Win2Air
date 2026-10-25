#include "LatencyController.h"
namespace Win2Air {
PresentationConfig RequestedPresentation(std::string mode, int custom_ms,
                                         int offset_ms) {
  AppSettings settings;
  settings.latency_mode = std::move(mode);
  settings.custom_ms = custom_ms;
  ReceiverRecord receiver;
  receiver.offset_ms = offset_ms;
  return InitialPresentation(settings, receiver);
}
}  // namespace Win2Air
