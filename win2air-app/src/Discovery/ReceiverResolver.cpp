#include "ReceiverResolver.h"

#include <cctype>
#include <charconv>
#include <chrono>

#include "../Audio/AudioClock.h"
namespace Win2Air {
namespace {
struct HostQuery : OVERLAPPED {
  HANDLE cancel{};
  ADDRINFOEXW *addresses{};
  WSADATA winsock{};
  std::shared_ptr<HostQuery> lifetime;
  std::wstring host, port;
  std::mutex mutex;
  std::condition_variable ready;
  bool done{};
  DWORD error{};
  HostQuery() : OVERLAPPED{} {
    auto status = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (status)
      throw std::runtime_error("Initialize host resolver failed: " +
                               std::to_string(status));
  }
  ~HostQuery() {
    if (addresses) FreeAddrInfoExW(addresses);
    WSACleanup();
  }
};
void CALLBACK HostComplete(DWORD error, DWORD, OVERLAPPED *overlap) noexcept {
  auto &task = *static_cast<HostQuery *>(overlap);
  std::shared_ptr<HostQuery> retain;
  std::lock_guard lock(task.mutex);
  retain = task.lifetime;
  task.error = error;
  task.done = true;
  task.lifetime.reset();
  task.ready.notify_all();
}
uint64_t Features(std::string value) {
  auto parse = [](std::string const &text) -> uint64_t {
    std::string_view digits = text;
    if (digits.starts_with("0x") || digits.starts_with("0X"))
      digits.remove_prefix(2);
    uint64_t number{};
    auto result = std::from_chars(digits.data(), digits.data() + digits.size(),
                                  number, 16);
    if (digits.empty() || result.ec != std::errc{} ||
        result.ptr != digits.data() + digits.size())
      throw std::runtime_error("DNS-SD feature bitfield is malformed");
    return number;
  };
  auto separator = value.find(',');
  if (separator == std::string::npos) return parse(value);
  auto low = parse(value.substr(0, separator)),
       high = parse(value.substr(separator + 1));
  if (low > UINT32_MAX || high > UINT32_MAX)
    throw std::runtime_error("DNS-SD feature words exceed their bounds");
  return low | (high << 32);
}
}  // namespace
std::vector<std::pair<std::string, unsigned>> ResolveHost(
    std::string const &host, unsigned port, unsigned timeout_ms,
    std::function<bool()> canceled) {
  if (host.empty() || host.size() > 1024 ||
      host.find('\0') != std::string::npos || !port || port > 65535 ||
      !timeout_ms || timeout_ms > 10000)
    throw std::runtime_error(
        "Host resolution requires a valid host, port and bounded deadline");
  auto task = std::make_shared<HostQuery>();
  task->host = winrt::to_hstring(host).c_str();
  task->port = std::to_wstring(port);
  task->lifetime = task;
  ADDRINFOEXW hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  timeval timeout{static_cast<long>(timeout_ms / 1000),
                  static_cast<long>((timeout_ms % 1000) * 1000)};
  auto status = GetAddrInfoExW(task->host.c_str(), task->port.c_str(), NS_DNS,
                               nullptr, &hints, &task->addresses, &timeout,
                               task.get(), HostComplete, &task->cancel);
  if (status != WSA_IO_PENDING) {
    task->lifetime.reset();
    if (status)
      throw std::runtime_error("Resolve receiver host failed: " +
                               std::to_string(status));
    task->done = true;
  }
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock lock(task->mutex);
  while (!task->done) {
    if ((canceled && canceled()) ||
        std::chrono::steady_clock::now() >= deadline) {
      lock.unlock();
      status = GetAddrInfoExCancel(&task->cancel);
      throw std::runtime_error(
          "Host resolution canceled or exceeded deadline; cancel status=" +
          std::to_string(status));
    }
    task->ready.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (task->error)
    throw std::runtime_error("Resolve receiver host failed: " +
                             std::to_string(task->error));
  std::vector<std::pair<std::string, unsigned>> result;
  for (auto address = task->addresses; address; address = address->ai_next) {
    char numeric[INET6_ADDRSTRLEN]{};
    unsigned scope{};
    void *bytes{};
    if (address->ai_family == AF_INET)
      bytes = &reinterpret_cast<sockaddr_in *>(address->ai_addr)->sin_addr;
    else if (address->ai_family == AF_INET6) {
      auto v6 = reinterpret_cast<sockaddr_in6 *>(address->ai_addr);
      bytes = &v6->sin6_addr;
      scope = v6->sin6_scope_id;
    } else
      continue;
    if (!InetNtopA(address->ai_family, bytes, numeric, sizeof(numeric)))
      winrt::throw_last_error();
    result.emplace_back(numeric, scope);
    if (result.size() >= 16) break;
  }
  if (result.empty())
    throw std::runtime_error(
        "Receiver hostname resolved to no supported address");
  return result;
}
ResolvedReceiver ReceiverFromService(ResolvedService const &service,
                                     uint64_t epoch) {
  ResolvedReceiver value;
  if (!epoch || !service.port || service.port > 65535 ||
      service.addresses.empty())
    throw std::runtime_error("Resolved service lacks a real endpoint");
  auto identity = service.txt.find("deviceid"),
       model = service.txt.find("model");
  if (model == service.txt.end()) model = service.txt.find("am");
  if (identity == service.txt.end())
    throw std::runtime_error(
        "Service announcement lacks public receiver identity");
  value.record.receiver_id = identity->second;
  for (auto &c : value.record.receiver_id)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (!ValidReceiverIdentity(value.record.receiver_id))
    throw std::runtime_error("Service receiver identity is malformed");
  value.record.name =
      service.instance.substr(0, service.instance.find("._airplay."));
  value.record.model = model == service.txt.end() ? "" : model->second;
  auto manufacturer = service.txt.find("manufacturer");
  if (manufacturer == service.txt.end()) manufacturer = service.txt.find("mf");
  if (manufacturer != service.txt.end())
    value.record.manufacturer = manufacturer->second;
  auto features = service.txt.find("features");
  if (features != service.txt.end())
    value.record.features = Features(features->second);
  value.endpoint = {service.host,
                    service.addresses.front().first,
                    service.port,
                    service.interface_index,
                    service.addresses.front().second,
                    epoch,
                    MonotonicNs()};
  return value;
}
std::optional<ReceiverEndpoint> KnownEndpoint(
    std::vector<ReceiverRecord> const &records, std::string const &host,
    unsigned port) {
  if (host.empty() || host.find('\0') != std::string::npos || !port ||
      port > 65535)
    throw std::runtime_error("Manual receiver requires a valid host and port");
  auto canonical = [](std::string value) {
    if (!value.empty() && value.back() == '.') value.pop_back();
    for (auto &c : value)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
  };
  auto wanted = canonical(host);
  for (auto const &record : records) {
    for (auto const &endpoint : record.discovered_endpoints)
      if (endpoint.port == port && canonical(endpoint.host) == wanted &&
          !endpoint.resolved_address.empty())
        return endpoint;
    if (record.manual_endpoint && record.manual_endpoint->port == port &&
        canonical(record.manual_endpoint->host) == wanted &&
        !record.manual_endpoint->resolved_address.empty())
      return record.manual_endpoint;
  }
  ReceiverRecord const *named{};
  for (auto const &record : records) {
    if (canonical(record.name) != wanted) continue;
    if (named)
      throw std::runtime_error(
          "Receiver name is ambiguous; use its hostname or address");
    named = &record;
  }
  if (named) {
    for (auto const &endpoint : named->discovered_endpoints)
      if (endpoint.port == port && !endpoint.resolved_address.empty())
        return endpoint;
    if (named->manual_endpoint && named->manual_endpoint->port == port &&
        !named->manual_endpoint->resolved_address.empty())
      return named->manual_endpoint;
    throw std::runtime_error(
        "Receiver name has no endpoint on the requested port");
  }
  return {};
}
ResolvedReceiver ProbeReceiver(SenderBackend &library, sender_runtime *runtime,
                               std::string const &host, unsigned port,
                               uint64_t probe_id,
                               std::function<bool()> canceled) {
  auto deadline = MonotonicNs() + 5000000000;
  auto addresses = ResolveHost(host, port, 2000, canceled);
  sender_error error{};
  for (auto const &address : addresses) {
    auto callback = [](void *context) noexcept -> int {
      try {
        return (*static_cast<std::function<bool()> *>(context))() ? 1 : 0;
      } catch (...) {
        return 1;
      }
    };
    std::function<bool()> cancel = canceled ? canceled : [] { return false; };
    auto now = MonotonicNs();
    if (now >= deadline)
      throw std::runtime_error(
          "Receiver public info probe exceeded its total deadline");
    auto remaining = static_cast<uint32_t>(std::max<uint64_t>(
        1, std::min<uint64_t>(2000, (deadline - now) / 1000000)));
    sender_probe_request request{sizeof(request),
                                 1,
                                 address.first.c_str(),
                                 static_cast<uint16_t>(port),
                                 address.second,
                                 probe_id,
                                 remaining,
                                 callback,
                                 &cancel};
    sender_probe_info info{};
    info.size = sizeof(info);
    info.version = 1;
    auto result = library.Probe(runtime, &request, &info, &error);
    if (result == SENDER_CANCELED)
      throw std::runtime_error("Receiver public info probe canceled");
    if (result == SENDER_OK) {
      ResolvedReceiver value;
      value.record.receiver_id = info.receiver_id;
      value.record.name = info.name;
      value.record.model = info.model;
      value.record.firmware_version = info.firmware;
      if (info.features_available) value.record.features = info.features;
      value.endpoint = {host,           address.first, port,         0,
                        address.second, probe_id,      MonotonicNs()};
      return value;
    }
    if (result != SENDER_NETWORK && result != SENDER_TIMEOUT) break;
  }
  throw std::runtime_error(std::string(error.message) +
                           "; platform=" + std::to_string(error.platform_code) +
                           "; peer=" + std::to_string(error.peer_code));
}
}  // namespace Win2Air
