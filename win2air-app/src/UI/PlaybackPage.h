#pragma once
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <functional>
#include <optional>

#include "LocalizedResources.h"
#include "ViewModels/PlaybackViewModel.h"
namespace Win2Air {
class PlaybackPage {
  PlaybackViewModel model_;
  winrt::Microsoft::UI::Xaml::FrameworkElement root_{nullptr};
  LocalizedResources resources_;
  bool refreshing_{};
  uint64_t media_revision_{};
  std::string card_xaml_;
  std::vector<std::string> card_ids_;
  std::vector<std::string> source_names_;
  std::string logged_metadata_error_, logged_discovery_error_;
  std::vector<winrt::Microsoft::UI::Xaml::FrameworkElement> cards_;
  std::string selected_id_;
  PairingPrompt pairing_prompt_;
  std::optional<std::string> pending_pairing_;
  std::function<void()> open_devices_;
  std::function<void()> open_audio_;
  FailurePrompt blocking_;
  bool SessionOwnsSelection() const;
  void ResolveSelection(std::vector<ReceiverViewModel> const& views);
  void RenderCards(std::vector<ReceiverViewModel> const& views);
  void ShowDestinationVolume();
  template <class T>
  T Find(wchar_t const* name) const {
    return root_.FindName(name).as<T>();
  }
  void Render();

 public:
  PlaybackPage(StreamCoordinator&, std::filesystem::path const& xaml,
               winrt::Microsoft::UI::Dispatching::DispatcherQueue queue);
  winrt::Microsoft::UI::Xaml::FrameworkElement Content() const { return root_; }
  void Refresh();
  void Present(PlaybackSnapshot snapshot);
  void SetOpenDevices(std::function<void()> open) {
    open_devices_ = std::move(open);
  }
  void SetOpenAudio(std::function<void()> open) {
    open_audio_ = std::move(open);
  }
  std::optional<std::string> TakePairingPrompt();
  void DismissPairingPrompt(std::string const& identity) {
    if (pending_pairing_ == identity) pending_pairing_.reset();
    pairing_prompt_.Dismiss(model_.Snapshot().session, identity);
  }
  std::future<CommandResult> Pair(std::string const& identity,
                                  SecureBuffer pin) {
    return model_.Pair(identity, std::move(pin));
  }
  std::string LayoutState() const;
};
}  // namespace Win2Air
