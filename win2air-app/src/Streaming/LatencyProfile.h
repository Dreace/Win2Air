#pragma once
#include <stdexcept>
#include "../Settings/AppSettings.h"
#include "PresentationConfig.h"
namespace Win2Air {
inline std::string LatencyNetworkContext(ReceiverEndpoint const& endpoint) {
  if (!endpoint.interface_index || endpoint.resolved_address.empty()) return {};
  return endpoint.resolved_address + ":" + std::to_string(endpoint.port) +
         "|if=" + std::to_string(endpoint.interface_index) +
         "|scope=" + std::to_string(endpoint.scope_id);
}
inline std::string LatencyFirmwareKey(ReceiverRecord const& receiver) {
  if (!receiver.firmware_version.empty()) return receiver.firmware_version;
  if (!receiver.model.empty()) return "model:" + receiver.model;
  return {};
}
inline std::optional<ReceiverEndpoint> SelectedEndpoint(
    ReceiverRecord const& receiver) {
  if (receiver.manual_endpoint) return receiver.manual_endpoint;
  if (!receiver.discovered_endpoints.empty())
    return receiver.discovered_endpoints.front();
  return {};
}
inline std::optional<int> SavedLatency(ReceiverRecord const& receiver,
                                       ReceiverEndpoint const& endpoint) {
  if (!receiver.latency_profile) return {};
  auto const& profile = *receiver.latency_profile;
  auto key = LatencyFirmwareKey(receiver);
  auto context = LatencyNetworkContext(endpoint);
  if (key.empty() || context.empty() || profile.firmware != key ||
      profile.network_context != context ||
      profile.budget_ms < SENDER_MIN_PRESENTATION_MS ||
      profile.budget_ms > SENDER_MAX_PRESENTATION_MS ||
      profile.budget_ms + receiver.offset_ms < SENDER_MIN_PRESENTATION_MS ||
      profile.budget_ms + receiver.offset_ms > SENDER_MAX_PRESENTATION_MS)
    return {};
  return profile.budget_ms;
}
inline std::optional<int> RecentAdaptiveBudget(
    AppSettings const& settings, std::vector<ReceiverRecord> const& receivers,
    std::optional<int> live_base) {
  if (settings.latency_mode == "adaptive" && live_base) {
    if (*live_base < SENDER_MIN_PRESENTATION_MS ||
        *live_base > SENDER_MAX_PRESENTATION_MS)
      throw std::invalid_argument(
          "Live adaptive latency is outside the presentation range: " +
          std::to_string(*live_base));
    return *live_base;
  }
  // Historical transport targets carry no playback or cold-start evidence.
  (void)receivers;
  return {};
}
}  // namespace Win2Air
