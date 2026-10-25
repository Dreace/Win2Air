#include "StateRestore.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "../Streaming/PresentationConfig.h"
namespace Win2Air {
RestorePlan PlanRestore(SettingsSnapshot const& saved,
                        ActivationRequest const& request) {
  ValidateSettings(saved);
  RestorePlan plan;
  plan.page = request.page;
  plan.minimized =
      request.minimized || (request.startup && saved.settings.minimized);
  if (request.disconnect) return plan;
  auto selected =
      request.receiver_id.value_or(saved.settings.selected_receiver_id);
  if (selected.empty()) return plan;
  auto found = std::find_if(
      saved.receivers.begin(), saved.receivers.end(), [&](auto const& record) {
        return _stricmp(record.receiver_id.c_str(), selected.c_str()) == 0;
      });
  if (found == saved.receivers.end()) {
    if (request.receiver_id || saved.settings.restore_previous)
      throw std::runtime_error(
          "Saved receiver for activation or restoration is unavailable");
    return plan;
  }
  if (!request.receiver_id && !found->connect_established) return plan;
  if (!request.receiver_id && !saved.settings.restore_previous &&
      !found->auto_connect)
    return plan;
  plan.receiver_id = found->receiver_id;
  plan.automatic = !request.receiver_id;
  plan.presentation = InitialPresentation(saved.settings, *found);
  return plan;
}
WindowArea FitWindow(WindowPlacement const& saved, WindowArea const& area,
                     unsigned dpi) {
  if (area.width <= 0 || area.height <= 0 || dpi < 48 || dpi > 960 ||
      saved.width <= 0 || saved.height <= 0)
    throw std::runtime_error(
        "Window restoration requires a valid work area, size and DPI");
  auto width = std::clamp<int64_t>(int64_t(saved.width) * dpi / 96,
                                   std::min(area.width, 320), area.width);
  auto height = std::clamp<int64_t>(int64_t(saved.height) * dpi / 96,
                                    std::min(area.height, 240), area.height);
  auto x = std::clamp<int64_t>(
      saved.saved ? saved.x : area.x + (int64_t(area.width) - width) / 2,
      area.x, int64_t(area.x) + area.width - width);
  auto y = std::clamp<int64_t>(
      saved.saved ? saved.y : area.y + (int64_t(area.height) - height) / 2,
      area.y, int64_t(area.y) + area.height - height);
  return {static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
          static_cast<int>(height)};
}
}  // namespace Win2Air
