#include "ReceiverRegistry.h"

#include <algorithm>
#include <stdexcept>
namespace Win2Air {
namespace {
bool SameIdentity(std::string const &left, std::string const &right) {
  if (!ValidReceiverIdentity(left) || !ValidReceiverIdentity(right))
    return false;
  auto upper = [](char value) {
    return value >= 'a' && value <= 'f' ? char(value - 'a' + 'A') : value;
  };
  return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                    [&](char a, char b) { return upper(a) == upper(b); });
}
}  // namespace
ReceiverRegistry::ReceiverRegistry(std::vector<ReceiverRecord> saved)
    : records_(std::move(saved)) {
  SettingsSnapshot snapshot;
  snapshot.receivers = records_;
  ValidateSettings(snapshot);
}
void ReceiverRegistry::Merge(ReceiverRecord observed,
                             ReceiverEndpoint endpoint) {
  if (!ValidReceiverIdentity(observed.receiver_id) || observed.name.empty() ||
      observed.model.empty() || endpoint.resolved_address.empty() ||
      !endpoint.port || endpoint.port > 65535 || !endpoint.discovery_epoch)
    throw std::runtime_error(
        "Discovery requires public identity, real endpoint and epoch");
  auto found =
      std::find_if(records_.begin(), records_.end(), [&](auto const &record) {
        return SameIdentity(record.receiver_id, observed.receiver_id);
      });
  auto candidate = records_;
  size_t index = found == records_.end() ? records_.size()
                                         : size_t(found - records_.begin());
  if (index == records_.size()) {
    if (index >= 256)
      throw std::runtime_error("Receiver registry exceeded its storage bound");
    observed.identity_verified = false;
    observed.discovered_endpoints.clear();
    candidate.push_back(std::move(observed));
  }
  auto &record = candidate[index];
  if (index < records_.size()) {
    record.model = observed.model;
    if (!observed.manufacturer.empty())
      record.manufacturer = observed.manufacturer;
    if (record.firmware_version != observed.firmware_version)
      record.latency_profile.reset();
    record.firmware_version = observed.firmware_version;
    record.features = observed.features;
    if (observed.manual_endpoint)
      record.manual_endpoint = observed.manual_endpoint;
  }
  auto previous =
      std::find_if(record.discovered_endpoints.begin(),
                   record.discovered_endpoints.end(), [&](auto const &value) {
                     return value.host == endpoint.host &&
                            value.interface_index == endpoint.interface_index;
                   });
  if (previous != record.discovered_endpoints.end())
    *previous = std::move(endpoint);
  else {
    if (record.discovered_endpoints.size() >= 16)
      throw std::runtime_error("Receiver endpoint list exceeded its bound");
    record.discovered_endpoints.push_back(std::move(endpoint));
  }
  SettingsSnapshot snapshot;
  snapshot.receivers = candidate;
  ValidateSettings(snapshot);
  records_.swap(candidate);
}
void ReceiverRegistry::RemoveEndpoint(std::string const &identity,
                                      std::string const &address,
                                      unsigned port) {
  for (auto &record : records_)
    if (SameIdentity(record.receiver_id, identity))
      std::erase_if(record.discovered_endpoints, [&](auto const &endpoint) {
        return endpoint.resolved_address == address && endpoint.port == port;
      });
}
void ReceiverRegistry::Authenticate(std::string const &selected,
                                    std::string const &authenticated) {
  if (!SameIdentity(selected, authenticated))
    throw std::runtime_error(
        "Authenticated receiver identity differs from the selected public "
        "identity");
  for (auto &record : records_)
    if (SameIdentity(record.receiver_id, selected)) {
      record.identity_verified = true;
      return;
    }
  throw std::runtime_error(
      "Authenticated receiver is absent from the registry");
}
ReceiverRecord const &ReceiverRegistry::Find(
    std::string const &identity) const {
  for (auto const &record : records_)
    if (SameIdentity(record.receiver_id, identity)) return record;
  throw std::runtime_error("Selected receiver is unavailable");
}
void ReceiverRegistry::Update(ReceiverRecord record) {
  auto candidate = records_;
  auto found = std::find_if(
      candidate.begin(), candidate.end(), [&](auto const &current) {
        return SameIdentity(current.receiver_id, record.receiver_id);
      });
  if (found == candidate.end())
    throw std::runtime_error("Receiver to update is unavailable");
  if (record.identity_verified != found->identity_verified ||
      record.model != found->model ||
      record.manufacturer != found->manufacturer ||
      record.firmware_version != found->firmware_version ||
      record.features != found->features ||
      record.discovered_endpoints.size() != found->discovered_endpoints.size())
    throw std::runtime_error(
        "Receiver preferences cannot replace verified identity or "
        "capabilities");
  record.receiver_id = found->receiver_id;
  record.discovered_endpoints = found->discovered_endpoints;
  *found = std::move(record);
  SettingsSnapshot snapshot;
  snapshot.receivers = candidate;
  ValidateSettings(snapshot);
  records_.swap(candidate);
}
void ReceiverRegistry::Remove(std::string identity) {
  Find(identity);
  std::erase_if(records_, [&](auto const &record) {
    return SameIdentity(record.receiver_id, identity);
  });
}
void ReceiverRegistry::ForgetAuthentication(std::string const &identity) {
  auto canonical = Find(identity).receiver_id;
  for (auto &record : records_)
    if (record.receiver_id == canonical) record.identity_verified = false;
}
}  // namespace Win2Air
