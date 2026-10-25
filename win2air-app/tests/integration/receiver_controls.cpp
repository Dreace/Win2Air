#include "receiver_controls_support.h"

void TestReceiverControls(wchar_t const* directory) {
  auto root = std::filesystem::path(directory);
  Win2Air::Tests::PairingPromptScenario();
  Win2Air::Tests::OperationFeedbackScenario();
  Win2Air::Tests::SystemMediaScenario(root);
  Win2Air::Tests::VolumeInputScenario();
  Win2Air::Tests::ReceiverSessionScenario(root);
}
