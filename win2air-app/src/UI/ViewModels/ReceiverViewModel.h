#pragma once
#include <cmath>
#include <string>
#include "../../Discovery/ProductNames.h"
#include "../../Settings/Appearance.h"
#include "../../Streaming/StreamCoordinator.h"
namespace Win2Air {
inline bool IsValidReceiverPort(double value) {
  return std::isfinite(value) && value >= 1 && value <= 65535 &&
         std::floor(value) == value;
}
inline std::string DisplayModel(std::string const& model) {
  if (model.empty()) return {};
  return ProductName(model, resource_language.starts_with("zh"));
}
inline std::string ReceiverDetail(ReceiverRecord const& receiver) {
  std::string place;
  ReceiverEndpoint const* endpoint =
      receiver.manual_endpoint ? &*receiver.manual_endpoint
      : receiver.discovered_endpoints.empty()
          ? nullptr
          : &receiver.discovered_endpoints.front();
  if (endpoint) {
    place = endpoint->resolved_address.empty() ? endpoint->host
                                               : endpoint->resolved_address;
    if (!place.empty() && endpoint->port != 7000)
      place += ":" + std::to_string(endpoint->port);
  }
  auto model = ProductName(receiver.model, resource_language.starts_with("zh"),
                           receiver.manufacturer);
  if (model.empty()) return place;
  if (place.empty()) return model;
  return model + " | " + place;
}
struct ReceiverViewModel {
  ReceiverRecord receiver;
  SessionState state{SessionState::disconnected};
  bool active{}, pending{};
  std::string health{"unknown"};
  std::optional<unsigned> applied_volume;
  std::string recovery_reason, failure_kind;
  std::optional<unsigned> retry_seconds;
  bool adjustable{};
};
inline void AppendFault(std::string& text, std::string const& line) {
  if (line.empty()) return;
  if (!text.empty()) text += '\n';
  text += line;
}
inline std::string DeviceFaultText(ReceiverViewModel const& view,
                                   std::string const& authentication,
                                   std::string const& retry_label) {
  std::string text;
  if (view.failure_kind == "authentication") AppendFault(text, authentication);
  if (view.retry_seconds)
    AppendFault(text,
                retry_label + " " + std::to_string(*view.retry_seconds) + " s");
  return text;
}
std::vector<ReceiverViewModel> ReceiverViews(PlaybackSnapshot const& snapshot,
                                             bool include_hidden = false);
class ReceiverSelection {
  std::string projected_intent_;
  bool projected_connected_{};

 public:
  std::optional<int> Project(PlaybackSnapshot const& snapshot,
                             std::vector<ReceiverViewModel> const& visible);
};
class PairingPrompt {
  std::string receiver_;
  uint64_t intent_{};

 public:
  std::optional<std::string> Observe(ImmutableSessionSnapshot const& snapshot) {
    if (!snapshot || !snapshot->session ||
        snapshot->session->failure_kind != "authentication")
      return {};
    auto const& session = *snapshot->session;
    if (receiver_ == session.receiver_id && intent_ == session.intent_epoch)
      return {};
    receiver_ = session.receiver_id;
    intent_ = session.intent_epoch;
    return receiver_;
  }
  void Dismiss(ImmutableSessionSnapshot const& snapshot,
               std::string const& receiver) {
    if (snapshot && snapshot->session &&
        snapshot->session->receiver_id == receiver) {
      receiver_ = receiver;
      intent_ = snapshot->session->intent_epoch;
    }
  }
};
}  // namespace Win2Air
