#pragma once
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include "LocalizedResources.h"
#include "ViewModels/SettingsViewModel.h"
namespace Win2Air {
class SettingsPage {
  SettingsViewModel model_;
  LocalizedResources resources_;
  winrt::Microsoft::UI::Xaml::FrameworkElement root_{nullptr};
  std::vector<std::string> source_ids_;
  bool rendering_{};
  bool appearance_busy_{};
  std::string input_error_;
  FailurePrompt blocking_;
  template <class T>
  T Find(wchar_t const* name) const {
    return root_.FindName(name).as<T>();
  }
  void Input(std::function<void()> action);
  void Apply(bool appearance_busy);

 public:
  SettingsPage(
      StreamCoordinator&, std::filesystem::path const&,
      std::shared_ptr<SettingsDraft> draft = std::make_shared<SettingsDraft>());
  auto Content() const { return root_; }
  bool Pending() const { return model_.Pending(); }
  void SelectSection(int index);
  void Refresh(bool appearance_busy);
  void Present(PlaybackSnapshot snapshot);
};
}  // namespace Win2Air
