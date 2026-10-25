#pragma once
#include <windows.h>
#undef GetCurrentTime
#include "../../Streaming/StreamCoordinator.h"
#include "OperationFeedback.h"
#include "ReceiverViewModel.h"
#include "ViewModelBase.h"
#include "VolumeInput.h"
namespace Win2Air {
class PlaybackViewModel {
  StreamCoordinator& service_;
  winrt::com_ptr<ViewModelBase> projection_;
  OperationFeedback feedback_;
  PlaybackSnapshot snapshot_;
  ReceiverSelection selection_;
  std::map<std::string, VolumeInput> volumes_;
  std::map<std::string, uint64_t> volume_revisions_;

 public:
  PlaybackViewModel(StreamCoordinator& service,
                    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue)
      : service_(service),
        projection_(winrt::make_self<ViewModelBase>(queue)) {}
  ViewModelBase& Projection() const { return *projection_.get(); }
  void Refresh();
  void Present(PlaybackSnapshot snapshot);
  PlaybackSnapshot const& Snapshot() const { return snapshot_; }
  std::string Error() const;
  std::string BlockingError() const;
  std::string const& OperationError(std::string const& identity = {}) const {
    return feedback_.Error(identity);
  }
  bool Pending() const noexcept { return feedback_.Pending(); }
  void InputError(std::string message, std::string const& identity = {}) {
    feedback_.Report(std::move(message), identity);
  }
  std::optional<int> Selection(std::vector<ReceiverViewModel> const& visible) {
    return selection_.Project(snapshot_, visible);
  }
  void Connect(size_t receiver, SecureBuffer pin);
  void Disconnect();
  void RefreshReceivers() { feedback_.Submit(service_.RefreshReceivers()); }
  void Source(int index);
  void Local(bool value);
  void Add(std::string host, unsigned port);
  void ConnectIdentity(std::string identity, SecureBuffer pin = {});
  std::future<CommandResult> Pair(std::string const& identity,
                                  SecureBuffer pin) {
    feedback_.Begin(identity);
    return service_.Connect(identity, std::move(pin));
  }
  void Volume(std::string identity, double value);
  void Mute(std::string identity, bool value);
  void Update(ReceiverRecord const& receiver);
  void Endpoint(std::string identity, std::string host, unsigned port);
  void Delete(std::string identity);
  void Forget(std::string identity);
  void ResetAdaptiveLatency(std::string identity);
};
}  // namespace Win2Air
