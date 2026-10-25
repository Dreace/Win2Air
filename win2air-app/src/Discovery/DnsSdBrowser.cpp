#include "DnsSdBrowser.h"

#include <algorithm>
#include <atomic>
#include <chrono>
namespace Win2Air {
bool EnqueueAnnouncement(std::map<std::string, ServiceAnnouncement> &pending,
                         ServiceAnnouncement event) {
  if (pending.size() >= 256 && !pending.contains(event.instance)) return false;
  auto instance = event.instance;
  pending.insert_or_assign(std::move(instance), std::move(event));
  return true;
}
struct DnsSdBrowser::Query {
  DNS_SERVICE_CANCEL cancel{};
  DNS_SERVICE_BROWSE_REQUEST request{};
  std::wstring name;
  std::shared_ptr<Query> lifetime;
  std::mutex mutex;
  std::condition_variable complete;
  std::vector<ServiceAnnouncement> events;
  uint64_t epoch{};
  bool closed{}, finished{};
  DWORD error{};
};
void WINAPI DnsSdBrowser::BrowseComplete(DWORD status, void *context,
                                         DNS_RECORD *records) noexcept {
  auto &query = *static_cast<Query *>(context);
  std::shared_ptr<Query> retain;
  try {
    std::lock_guard lock(query.mutex);
    retain = query.lifetime;
    if (status == ERROR_CANCELLED) {
      query.finished = true;
      query.lifetime.reset();
      query.complete.notify_all();
    } else if (!query.closed) {
      if (status != ERROR_SUCCESS)
        query.error = status;
      else
        for (auto record = records; record; record = record->pNext)
          if (record->wType == DNS_TYPE_PTR && record->Data.PTR.pNameHost) {
            if (query.events.size() >= 256) {
              query.error = ERROR_BUFFER_OVERFLOW;
              break;
            }
            auto instance = winrt::to_string(record->Data.PTR.pNameHost);
            if (instance.find("._airplay._tcp.local") == std::string::npos &&
                instance.find("._raop._tcp.local") == std::string::npos)
              continue;
            query.events.push_back(
                {std::move(instance), record->dwTtl == 0, query.epoch});
          }
    }
  } catch (...) {
    std::lock_guard lock(query.mutex);
    query.error = ERROR_NOT_ENOUGH_MEMORY;
  }
  if (records) DnsRecordListFree(records, DnsFreeRecordList);
}
DnsSdBrowser::~DnsSdBrowser() {
  try {
    Stop();
  } catch (...) {
  }
}
void DnsSdBrowser::Start(uint64_t epoch) {
  if (!epoch || !queries_.empty())
    throw std::runtime_error(
        "Discovery requires a stopped browser and explicit epoch");
  try {
    for (auto name : {L"_airplay._tcp.local", L"_raop._tcp.local"}) {
      auto query = std::make_shared<Query>();
      query->name = name;
      query->epoch = epoch;
      query->lifetime = query;
      query->request.Version = DNS_QUERY_REQUEST_VERSION1;
      query->request.QueryName = query->name.c_str();
      query->request.pBrowseCallback = BrowseComplete;
      query->request.pQueryContext = query.get();
      auto status = DnsServiceBrowse(&query->request, &query->cancel);
      if (status != DNS_REQUEST_PENDING) {
        query->lifetime.reset();
        throw std::runtime_error("Start DNS-SD browse failed: " +
                                 std::to_string(status));
      }
      queries_.push_back(std::move(query));
    }
  } catch (...) {
    Stop();
    throw;
  }
}
void CancelBrowses(std::vector<BrowseCancellation> const &queries,
                   BrowseDeadline deadline) {
  DWORD failure{};
  std::vector<bool> pending;
  for (auto const &query : queries) {
    auto status = query.cancel();
    pending.push_back(status == ERROR_SUCCESS);
    if (status != ERROR_SUCCESS) failure = status;
  }
  for (size_t index = 0; index < queries.size(); ++index)
    if (pending[index] && !queries[index].wait(deadline))
      failure = ERROR_TIMEOUT;
  if (failure)
    throw std::runtime_error("Cancel DNS-SD browse failed: " +
                             std::to_string(failure));
}
void DnsSdBrowser::Stop(BrowseDeadline deadline) {
  std::vector<BrowseCancellation> pending;
  for (auto const &query : queries_) {
    {
      std::lock_guard lock(query->mutex);
      query->closed = true;
    }
    pending.push_back(
        {[query] { return DnsServiceBrowseCancel(&query->cancel); },
         [query](BrowseDeadline limit) {
           std::unique_lock lock(query->mutex);
           return query->complete.wait_until(lock, limit,
                                             [&] { return query->finished; });
         }});
  }
  queries_.clear();
  CancelBrowses(pending, deadline);
}
std::vector<ServiceAnnouncement> DnsSdBrowser::Take() {
  std::vector<ServiceAnnouncement> result;
  for (auto const &query : queries_) {
    std::lock_guard lock(query->mutex);
    if (query->error)
      throw std::runtime_error("DNS-SD browse callback failed: " +
                               std::to_string(query->error));
    result.insert(result.end(), std::make_move_iterator(query->events.begin()),
                  std::make_move_iterator(query->events.end()));
    query->events.clear();
  }
  return result;
}
namespace {
struct Resolution {
  DNS_SERVICE_CANCEL cancel{};
  DNS_SERVICE_RESOLVE_REQUEST request{};
  std::wstring name;
  std::shared_ptr<Resolution> lifetime;
  std::mutex mutex;
  std::condition_variable ready;
  bool done{};
  DWORD error{};
  ResolvedService value;
};
void WINAPI ResolveComplete(DWORD status, void *context,
                            DNS_SERVICE_INSTANCE *instance) noexcept {
  auto &task = *static_cast<Resolution *>(context);
  std::shared_ptr<Resolution> retain;
  try {
    std::lock_guard lock(task.mutex);
    retain = task.lifetime;
    task.error = status;
    if (status == ERROR_SUCCESS && instance) {
      if (!instance->pszInstanceName || !instance->pszHostName ||
          !instance->wPort || instance->dwPropertyCount > 256)
        throw std::runtime_error("Incomplete DNS-SD service");
      task.value.instance = winrt::to_string(instance->pszInstanceName);
      task.value.host = winrt::to_string(instance->pszHostName);
      task.value.port = instance->wPort;
      task.value.interface_index = instance->dwInterfaceIndex;
      char address[INET6_ADDRSTRLEN]{};
      if (instance->ip4Address) {
        if (!InetNtopA(AF_INET, instance->ip4Address, address, sizeof(address)))
          winrt::throw_last_error();
        task.value.addresses.push_back({address, 0});
      }
      if (instance->ip6Address) {
        if (!InetNtopA(AF_INET6, instance->ip6Address, address,
                       sizeof(address)))
          winrt::throw_last_error();
        auto scope = IN6_IS_ADDR_LINKLOCAL(
                         reinterpret_cast<IN6_ADDR *>(instance->ip6Address))
                         ? instance->dwInterfaceIndex
                         : 0;
        task.value.addresses.push_back({address, scope});
      }
      for (DWORD i = 0; i < instance->dwPropertyCount; ++i) {
        if (!instance->keys || !instance->values || !instance->keys[i] ||
            !instance->values[i])
          throw std::runtime_error("Incomplete DNS-SD TXT property");
        auto key = winrt::to_string(instance->keys[i]),
             value = winrt::to_string(instance->values[i]);
        if (key.empty() || key.size() > 255 || value.size() > 4096 ||
            !task.value.txt.emplace(std::move(key), std::move(value)).second)
          throw std::runtime_error("Invalid or duplicated DNS-SD TXT property");
      }
    } else if (status == ERROR_SUCCESS)
      task.error = ERROR_INVALID_DATA;
    task.done = true;
    task.lifetime.reset();
    task.ready.notify_all();
  } catch (...) {
    std::lock_guard lock(task.mutex);
    task.error = ERROR_INVALID_DATA;
    task.done = true;
    task.lifetime.reset();
    task.ready.notify_all();
  }
  if (instance) DnsServiceFreeInstance(instance);
}
}  // namespace
ResolvedService ResolveService(std::string const &instance,
                               unsigned interface_index, unsigned timeout_ms,
                               std::function<bool()> canceled) {
  if (instance.empty() || instance.size() > 1024 ||
      instance.find('\0') != std::string::npos || !timeout_ms ||
      timeout_ms > 10000)
    throw std::runtime_error(
        "Service resolution requires a bounded instance and deadline");
  auto task = std::make_shared<Resolution>();
  task->name = winrt::to_hstring(instance).c_str();
  task->lifetime = task;
  task->request.Version = DNS_QUERY_REQUEST_VERSION1;
  task->request.InterfaceIndex = interface_index;
  task->request.QueryName = task->name.data();
  task->request.pResolveCompletionCallback = ResolveComplete;
  task->request.pQueryContext = task.get();
  auto status = DnsServiceResolve(&task->request, &task->cancel);
  if (status != DNS_REQUEST_PENDING) {
    task->lifetime.reset();
    throw std::runtime_error("Start DNS-SD resolution failed: " +
                             std::to_string(status));
  }
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock lock(task->mutex);
  while (!task->done) {
    if ((canceled && canceled()) ||
        std::chrono::steady_clock::now() >= deadline) {
      lock.unlock();
      status = DnsServiceResolveCancel(&task->cancel);
      throw std::runtime_error(
          "DNS-SD resolution canceled or exceeded deadline; cancel status=" +
          std::to_string(status));
    }
    task->ready.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (task->error)
    throw std::runtime_error("DNS-SD resolution failed: " +
                             std::to_string(task->error));
  return std::move(task->value);
}
}  // namespace Win2Air
