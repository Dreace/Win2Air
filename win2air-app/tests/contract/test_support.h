#pragma once
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
template <class F>
void WaitUntil(F predicate, char const* behavior) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  throw std::runtime_error(behavior);
}
inline void Require(bool ok, char const* behavior) {
  if (!ok) throw std::runtime_error(behavior);
}
template <class F>
void Rejects(F action, char const* behavior) {
  bool failed = false;
  try {
    action();
  } catch (...) {
    failed = true;
  }
  Require(failed, behavior);
}
void TestSenderAbi(wchar_t const* path);
void TestSettingsCredentials(wchar_t const* path);
void TestCommandLifecycle(wchar_t const* path);
void TestLogger(wchar_t const* path);
void TestBorrowedCallbacks(wchar_t const* path);
void TestCaptureSource();
void TestAudioConversion();
void TestLocalPlaybackLease(wchar_t const* path);
void TestReceiverResolution();
void DiscoverReceivers(wchar_t const* dll);
void TestSystemAudioSession(wchar_t const* dll, wchar_t const* root);
void TestReceiverControls(wchar_t const* root);
void TestApplicationActivation();
void TestTrayLifecycle();
void TestStartupRestore(wchar_t const* root);
void TestSilenceStandby();
void TestPresentationStandby(wchar_t const* directory);
void TestPresentationMeasurement();
void TestReconnectBackoff();
void TestRecoveryFaults(wchar_t const* directory);
void TestMetricsHealth();
void TestDiagnosticsSecrets(wchar_t const* directory);
void TestMetricsDiagnostics(wchar_t const* directory);
void TestSettingsRoundtrip(wchar_t const* directory);
void TestForgetPairing(wchar_t const* directory);
void TestSettingsAccessibility(wchar_t const* directory);
void TestStorePreview();
void TestAdaptiveLatency();
int MeasurePresentation(wchar_t** arguments);
