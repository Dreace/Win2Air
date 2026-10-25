#pragma once
#include <chrono>
#include <filesystem>
#include <future>

#include "../../src/Streaming/StreamCoordinator.h"
#include "../../src/UI/ViewModels/OperationFeedback.h"
#include "../../src/UI/ViewModels/ReceiverViewModel.h"
#include "../../src/UI/ViewModels/TrayViewModel.h"
#include "../../src/UI/ViewModels/VolumeInput.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"

// Scenarios of TestReceiverControls, in call order.
namespace Win2Air::Tests {
template <class Future>
CommandResult Complete(Future future) {
  Require(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "Receiver command exceeded its deadline");
  return future.get();
}
inline SettingsSnapshot SavedReceivers(std::filesystem::path const& root) {
  SettingsStore store(root);
  auto saved = store.Load();
  saved.settings.revision = 1;
  for (auto identity : {"AA:BB:CC:DD:EE:FF", "11:22:33:44:55:66"}) {
    ReceiverRecord record;
    record.receiver_id = identity;
    record.name = "Equal display name";
    record.model = "Virtual receiver";
    record.features =
        (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
    record.manual_endpoint = ReceiverEndpoint{"127.0.0.1", "127.0.0.1", 7000};
    saved.receivers.push_back(record);
  }
  store.Save(saved);
  return saved;
}
void PairingPromptScenario();
void OperationFeedbackScenario();
void SystemMediaScenario(std::filesystem::path const& root);
void VolumeInputScenario();
void ReceiverSessionScenario(std::filesystem::path const& root);
}  // namespace Win2Air::Tests
