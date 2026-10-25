#include <iostream>
#include <thread>

#include "../../src/Discovery/ProductNames.h"
#include "../../src/Discovery/ReceiverRegistry.h"
#include "../../src/Discovery/ReceiverResolver.h"
#include "test_support.h"
using namespace Win2Air;
void TestReceiverResolution() {
  Require(ProductName("Mac14,10", true) == "MacBook Pro（16 英寸，2023 年）" &&
              ProductName("Mac15,10", false) == "MacBook Pro (14-inch, 2023)" &&
              ProductName("MacBookAir10,1", false) ==
                  "MacBook Air (13-inch, 2020)" &&
              ProductName("MacBookPro15,2", true) ==
                  "MacBook Pro（13 英寸，2018/2019 年）",
          "MacBook labels keep family, display size and year without hardware "
          "configuration details");
  Require(ProductName("", false, "Sonos").empty() &&
              ProductName("   ", false, "Sonos") == "   ",
          "Missing model identifiers never select a manufacturer product");
  Require(ProductName("AudioAccessory5,1", false).starts_with("HomePod mini") &&
              ProductName("MacBookPro18,1", false).find("MacBook Pro") !=
                  std::string::npos,
          "Embedded identities resolve speakers and Mac receivers without "
          "source catalogs");
  Require(ProductName("S39", false, "SONOS") == "Era 100" &&
              ProductName("S39", false, "Other") == "S39" &&
              ProductName("S39", false) == "S39",
          "Third party model labels require the advertised manufacturer");
  Require(
      ProductName("S39", false, " sonos ") == "Era 100" &&
          ProductName("Unknown receiver", true, "Sonos") == "Unknown receiver",
      "Manufacturer aliases retain exact model matching and unknown "
      "identifiers");
  std::map<std::string, ServiceAnnouncement> pending;
  for (unsigned i = 0; i < 1024; ++i)
    Require(EnqueueAnnouncement(pending, {std::to_string(i), false, 1}) ==
                (i < 256),
            "Discovery rejects excess new names before storing them");
  Require(pending.size() == 256 &&
              EnqueueAnnouncement(pending, {"0", false, 2}) &&
              pending.at("0").epoch == 2,
          "A full discovery queue still updates existing announcements");
  pending.erase("0");
  Require(EnqueueAnnouncement(pending, {"replacement", false, 3}) &&
              pending.size() == 256,
          "Discovery continues accepting new names after draining an item");
  ResolvedService service{"Living room._airplay._tcp.local",
                          "receiver.local",
                          7000,
                          9,
                          {{"deviceid", "aa:bb:cc:dd:ee:ff"},
                           {"model", "AudioAccessory"},
                           {"features", "0x200,0x10000"}},
                          {{"fe80::1", 9}}};
  service.txt["mf"] = "Apple";
  auto resolved = ReceiverFromService(service, 1);
  Require(resolved.record.manufacturer == "Apple",
          "Discovery retains the advertised manufacturer");
  Require(resolved.endpoint.scope_id == 9 &&
              resolved.record.features == ((uint64_t(1) << 48) | 512),
          "TXT and scoped numeric address stay associated");
  ReceiverRegistry registry;
  registry.Merge(resolved.record, resolved.endpoint);
  registry.Merge(resolved.record, resolved.endpoint);
  Require(registry.Records().size() == 1 &&
              registry.Records()[0].discovered_endpoints.size() == 1 &&
              !registry.Records()[0].identity_verified,
          "Duplicate announcements merge without granting authentication");
  resolved.endpoint.resolved_address = "fe80::2";
  registry.Merge(resolved.record, resolved.endpoint);
  Require(registry.Find(resolved.record.receiver_id)
                  .discovered_endpoints[0]
                  .resolved_address == "fe80::2",
          "New address replaces discovery endpoint without changing identity");
  auto other = resolved;
  other.record.receiver_id = "00:11:22:33:44:55";
  registry.Merge(other.record, other.endpoint);
  Require(registry.Records().size() == 2,
          "Equal receiver names preserve distinct identities");
  Require(KnownEndpoint(registry.Records(), "RECEIVER.LOCAL.", 7000)
                  ->resolved_address == "fe80::2",
          "Manual host prefers matching real DNS-SD endpoint with canonical "
          "hostname");
  Rejects([&] { KnownEndpoint(registry.Records(), "Living room", 7000); },
          "Duplicate display names require an unambiguous host or address");
  Rejects(
      [&] {
        registry.Authenticate(resolved.record.receiver_id,
                              other.record.receiver_id);
      },
      "Mismatched authenticated identity rejected");
  auto saved = resolved.record;
  saved.receiver_id = "aa:bb:cc:dd:ee:ff";
  saved.identity_verified = true;
  saved.volume = 0.4;
  ReceiverRegistry restored({saved});
  restored.Merge(resolved.record, resolved.endpoint);
  Require(KnownEndpoint(restored.Records(), "Living room", 7000)
                  ->resolved_address == "fe80::2",
          "Unique discovered name resolves to its actual endpoint before "
          "public probing");
  Require(restored.Records().size() == 1 &&
              restored.Find(resolved.record.receiver_id).volume == 0.4 &&
              restored.Find(resolved.record.receiver_id).receiver_id ==
                  saved.receiver_id,
          "Discovery preserves saved identity spelling and preferences across "
          "hexadecimal case differences");
  restored.Authenticate(saved.receiver_id, resolved.record.receiver_id);
  restored.RemoveEndpoint(resolved.record.receiver_id,
                          resolved.endpoint.resolved_address,
                          resolved.endpoint.port);
  Require(restored.Find(saved.receiver_id).discovered_endpoints.empty(),
          "Endpoint removal matches the stable identity across hexadecimal "
          "case differences");
  service.txt.erase("features");
  Require(!ReceiverFromService(service, 2).record.features,
          "Absent features remain unknown");
  service.port = 0;
  Rejects([&] { ReceiverFromService(service, 2); }, "Zero port rejected");
  Require(!ResolveHost("127.0.0.1", 7000, 1000, {}).empty(),
          "Native host resolution returns a numeric address");
  Rejects([&] { ResolveHost("127.0.0.1", 65536, 1000, {}); },
          "Port overflow rejected before socket conversion");
}
void DiscoverReceivers(wchar_t const *dll) {
  SenderLibrary library(dll);
  sender_runtime *runtime{};
  sender_error error{};
  sender_create_options options{
      SENDER_ABI_VERSION,
      sizeof(options),
      {1, "Win2Air discovery", "Win2Air", 4096, nullptr, nullptr, nullptr},
      {1000, 0},
      nullptr};
  Require(library.Create(options, &runtime, &error) == SENDER_OK,
          "Create native probe runtime");
  try {
    DnsSdBrowser browser;
    browser.Start(1);
    std::vector<std::string> seen;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
      for (auto const &event : browser.Take())
        if (!event.removed &&
            std::find(seen.begin(), seen.end(), event.instance) == seen.end()) {
          seen.push_back(event.instance);
          try {
            auto service = ResolveService(event.instance, 0, 2000, {});
            Require(!service.addresses.empty(),
                    "DNS-SD receiver supplies a numeric address");
            auto address = service.addresses.front().first;
            if (service.addresses.front().second)
              address += "%" + std::to_string(service.addresses.front().second);
            auto info =
                ProbeReceiver(library, runtime, address, service.port, 1, {});
            std::cout << info.record.name << " | " << info.record.model << " | "
                      << info.record.receiver_id << " | "
                      << info.endpoint.resolved_address << ":"
                      << info.endpoint.port << " | features="
                      << (info.record.features
                              ? std::to_string(*info.record.features)
                              : "unknown")
                      << "\n";
          } catch (std::exception const &failure) {
            std::cout << event.instance << " | unavailable: " << failure.what()
                      << "\n";
          }
        }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    browser.Stop();
    Require(library.Destroy(runtime, &error) == SENDER_OK,
            "Release native probe runtime");
  } catch (...) {
    library.Destroy(runtime, &error);
    throw;
  }
}
