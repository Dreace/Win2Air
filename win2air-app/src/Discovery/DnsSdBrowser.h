#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windns.h>
#include <winrt/base.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
namespace Win2Air {
struct ServiceAnnouncement {
  std::string instance;
  bool removed{};
  uint64_t epoch{};
};
bool EnqueueAnnouncement(std::map<std::string, ServiceAnnouncement>& pending,
                         ServiceAnnouncement event);
struct ResolvedService {
  std::string instance, host;
  unsigned port{}, interface_index{};
  std::map<std::string, std::string> txt;
  std::vector<std::pair<std::string, unsigned>> addresses;
};
using BrowseDeadline = std::chrono::steady_clock::time_point;
struct BrowseCancellation {
  std::function<DWORD()> cancel;
  std::function<bool(BrowseDeadline)> wait;
};
void CancelBrowses(std::vector<BrowseCancellation> const& queries,
                   BrowseDeadline deadline);
class DnsSdBrowser {
  struct Query;
  std::vector<std::shared_ptr<Query>> queries_;
  static void WINAPI BrowseComplete(DWORD, void*, DNS_RECORD*) noexcept;

 public:
  virtual ~DnsSdBrowser();
  virtual void Start(uint64_t epoch);
  virtual void Stop(BrowseDeadline deadline = std::chrono::steady_clock::now() +
                                              std::chrono::seconds(2));
  virtual std::vector<ServiceAnnouncement> Take();
};
ResolvedService ResolveService(std::string const& instance,
                               unsigned interface_index, unsigned timeout_ms,
                               std::function<bool()> canceled);
}  // namespace Win2Air
