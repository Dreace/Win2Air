#pragma once
#include "../Settings/AppSettings.h"
#include "../Streaming/SenderLibrary.h"
#include "DnsSdBrowser.h"
namespace Win2Air {
struct ResolvedReceiver {
  ReceiverRecord record;
  ReceiverEndpoint endpoint;
};
std::vector<std::pair<std::string, unsigned>> ResolveHost(
    std::string const& host, unsigned port, unsigned timeout_ms,
    std::function<bool()> canceled);
ResolvedReceiver ReceiverFromService(ResolvedService const& service,
                                     uint64_t epoch);
std::optional<ReceiverEndpoint> KnownEndpoint(
    std::vector<ReceiverRecord> const&, std::string const& host, unsigned port);
ResolvedReceiver ProbeReceiver(SenderBackend& library, sender_runtime* runtime,
                               std::string const& host, unsigned port,
                               uint64_t probe_id,
                               std::function<bool()> canceled);
}  // namespace Win2Air
