#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "../../src/Settings/StorageIO.h"
#include "../../src/Streaming/LatencyProfile.h"
#include "../../src/Streaming/StreamCoordinator.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
#include "../fixtures/TransformFixture.h"

// Scenarios of TestPresentationStandby, in call order. Each one owns its
// virtual receiver, capture source and coordinator unless noted.
namespace Win2Air::Tests {
inline CommandResult Complete(std::future<CommandResult> value) {
  Require(value.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "Streaming settings operation must complete within its deadline");
  return value.get();
}
inline SettingsSnapshot Save(std::filesystem::path const& root) {
  SettingsStore store(root);
  auto saved = store.Load();
  saved.settings.revision = 1;
  ReceiverRecord record;
  record.receiver_id = "AA:BB:CC:DD:EE:FF";
  record.name = record.model = "Virtual timing receiver";
  record.features =
      (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
  record.manual_endpoint = ReceiverEndpoint{"127.0.0.1", "127.0.0.1", 7000};
  saved.receivers = {record};
  store.Save(saved);
  return saved;
}
void StandbyDisconnectScenario(std::filesystem::path const& root);
void ReceiverMediaControlScenario(std::filesystem::path const& root);
void ReceiverReportedLatencyScenario(std::filesystem::path const& root);
void AdaptiveLossRecoveryScenario(std::filesystem::path const& root);
void LiveSliderLatencyScenario(std::filesystem::path const& root);
void SavedAuthenticationScenario(std::filesystem::path const& root);
void ConversionChainScenario(std::filesystem::path const& root,
                             SettingsSnapshot const& saved);
void UltraFixedAdaptiveScenario(std::filesystem::path const& root,
                                std::string const& id);
void AdaptiveRetainedProfileScenario(std::filesystem::path const& root,
                                     std::string const& id);
void AdaptiveStandbyAnchorScenario(std::filesystem::path const& root,
                                   std::string const& id);
void AdaptiveRejectionRecoveryScenario(std::filesystem::path const& root,
                                       std::string const& id);
void MinimalLeadScenario(std::filesystem::path const& root,
                         std::string const& id);
void ContinuousDiscontinuityScenario(std::filesystem::path const& root,
                                     std::string const& id);
void DevicePositionClockScenario(std::filesystem::path const& root,
                                 std::string const& id);
void CapturePacketLossScenario(std::filesystem::path const& root,
                               std::string const& id);
void SourceErrorScenario(std::filesystem::path const& root,
                         std::string const& id);
}  // namespace Win2Air::Tests
