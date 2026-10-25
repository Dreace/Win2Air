#pragma once
#include "../Settings/AppSettings.h"
#include "../Streaming/SessionModels.h"
#include "Activation.h"
namespace Win2Air {
struct RestorePlan {
  AppPage page{AppPage::playback};
  bool minimized{}, automatic{};
  std::optional<std::string> receiver_id;
  std::optional<PresentationConfig> presentation;
};
RestorePlan PlanRestore(SettingsSnapshot const& saved,
                        ActivationRequest const& request);
struct WindowArea {
  int x{}, y{}, width{}, height{};
};
WindowArea FitWindow(WindowPlacement const& saved, WindowArea const& work_area,
                     unsigned dpi);
}  // namespace Win2Air
