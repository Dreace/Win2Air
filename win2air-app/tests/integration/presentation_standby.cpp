#include "presentation_standby_scenarios.h"

using namespace Win2Air;
using namespace Win2Air::Tests;
void TestPresentationStandby(wchar_t const* directory) {
  auto root = std::filesystem::path(directory);
  auto saved = Save(root);
  auto id = saved.receivers.front().receiver_id;
  StandbyDisconnectScenario(root);
  ReceiverMediaControlScenario(root);
  ReceiverReportedLatencyScenario(root);
  AdaptiveLossRecoveryScenario(root);
  LiveSliderLatencyScenario(root);
  SavedAuthenticationScenario(root);
  ConversionChainScenario(root, saved);
  UltraFixedAdaptiveScenario(root, id);
  AdaptiveRetainedProfileScenario(root, id);
  AdaptiveStandbyAnchorScenario(root, id);
  AdaptiveRejectionRecoveryScenario(root, id);
  MinimalLeadScenario(root, id);
  ContinuousDiscontinuityScenario(root, id);
  DevicePositionClockScenario(root, id);
  CapturePacketLossScenario(root, id);
  SourceErrorScenario(root, id);
}
