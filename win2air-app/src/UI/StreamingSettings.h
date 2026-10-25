#pragma once
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include "LocalizedResources.h"
#include "ViewModels/StreamingViewModel.h"
namespace Win2Air {
class StreamingSettings {
  StreamingViewModel model_;
  winrt::Microsoft::UI::Xaml::FrameworkElement root_{nullptr};
  LocalizedResources resources_;
  std::wstring input_error_;
  bool rendering_{};
  FailurePrompt blocking_;
  void ApplyLatency();
  template <class T>
  T Find(wchar_t const* name) {
    return root_.FindName(name).as<T>();
  }

 public:
  StreamingSettings(StreamCoordinator&, std::filesystem::path const& xaml,
                    std::shared_ptr<StreamingDraft> draft =
                        std::make_shared<StreamingDraft>());
  winrt::Microsoft::UI::Xaml::FrameworkElement Content() const { return root_; }
  void Refresh();
  bool Pending() const { return model_.Snapshot().pending; }
};
}  // namespace Win2Air
