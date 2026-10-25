#pragma once
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>

#include "../../Streaming/SessionModels.h"
namespace Win2Air {
struct ViewModelBase
    : winrt::implements<
          ViewModelBase,
          winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged> {
 private:
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;
  ImmutableSessionSnapshot snapshot_;

 public:
  explicit ViewModelBase(
      winrt::Microsoft::UI::Dispatching::DispatcherQueue queue)
      : queue_(queue) {
    if (!queue_) throw winrt::hresult_invalid_argument();
  }
  winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler>
      changed;
  winrt::event_token PropertyChanged(
      winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const&
          handler) {
    return changed.add(handler);
  }
  void PropertyChanged(winrt::event_token token) noexcept {
    changed.remove(token);
  }
  ImmutableSessionSnapshot Snapshot() const {
    if (!queue_.HasThreadAccess()) throw winrt::hresult_wrong_thread();
    return snapshot_;
  }
  winrt::hstring ErrorDetails() const {
    auto value = Snapshot();
    if (!value || value->last_command.status != CommandStatus::failed)
      return {};
    auto const& result = value->last_command;
    return winrt::to_hstring(
        result.message + "; command=" + std::to_string(result.command_id) +
        "; generation=" + std::to_string(result.generation) +
        "; platform=" + std::to_string(result.platform_code) +
        "; peer=" + std::to_string(result.peer_code));
  }
  void Post(ImmutableSessionSnapshot snapshot) {
    if (!snapshot) throw winrt::hresult_invalid_argument();
    auto weak = get_weak();
    if (!queue_.TryEnqueue([weak, snapshot = std::move(snapshot)] {
          if (auto self = weak.get()) {
            if (self->snapshot_ &&
                snapshot->revision <= self->snapshot_->revision)
              return;
            self->snapshot_ = snapshot;
            self->changed(
                *self,
                winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(
                    L"Snapshot"));
            self->changed(
                *self,
                winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs(
                    L"ErrorDetails"));
          }
        }))
      throw winrt::hresult_error(E_ABORT,
                                 L"UI dispatcher rejected snapshot projection");
  }
};
}  // namespace Win2Air
