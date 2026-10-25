#pragma once
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "LocalizedResources.h"
#include "ViewModels/PlaybackViewModel.h"
namespace Win2Air {
class DevicesPage {
  PlaybackViewModel model_;
  winrt::Microsoft::UI::Xaml::FrameworkElement root_{nullptr};
  LocalizedResources resources_;
  std::string card_xaml_;
  std::vector<std::string> card_ids_;
  std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> cards_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  bool confirmation_open_{};
  FailurePrompt blocking_;
  void RenderCards(std::vector<ReceiverViewModel> const& views);
  winrt::fire_and_forget ConfirmReceiverAction(std::string identity,
                                               bool forget);
  template <class T>
  T Find(wchar_t const* name) const {
    return root_.FindName(name).as<T>();
  }
  void Render();

 public:
  DevicesPage(StreamCoordinator&, std::filesystem::path const& xaml,
              winrt::Microsoft::UI::Dispatching::DispatcherQueue queue);
  ~DevicesPage() { *alive_ = false; }
  winrt::Microsoft::UI::Xaml::FrameworkElement Content() const { return root_; }
  void Refresh();
  void FocusAdd();
};
}  // namespace Win2Air
