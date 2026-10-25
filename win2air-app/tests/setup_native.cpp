#include "../src/Shell/TrayService.h"

#include <SystemMediaTransportControlsInterop.h>
#include <objbase.h>
#include <windows.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include "../src/App/SystemMediaSession.h"

#include <UIAutomation.h>
#include <appmodel.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <wrl/client.h>
#include <wtsapi32.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/Settings/SettingsStore.h"
#include "contract/test_support.h"
#include "native_gui.h"
int wmain(int argc, wchar_t **argv) {
  try {
    check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED),
          "Initialize test apartment");
    if (argc == 3 && std::wstring(argv[1]) == L"--publish-media-fixture") {
      publish_media_fixture(argv[2]);
    } else if (argc == 3 && std::wstring(argv[1]) == L"--assets") {
      validate_icon(std::filesystem::path(argv[2]) / L"StoreLogo.png", 50);
      validate_icon(std::filesystem::path(argv[2]) / L"Square44x44Logo.png",
                    44);
      validate_icon(std::filesystem::path(argv[2]) / L"Square150x150Logo.png",
                    150);
    } else if (argc == 3 && std::wstring(argv[1]) == L"--prepare-ui-review") {
      auto report = std::filesystem::path(argv[2]);
      if (!report.is_absolute())
        throw std::runtime_error("UI review requires an absolute report path");
      Win2Air::SettingsStore store(report.parent_path() / L"gui-state" /
                                   report.stem());
      auto saved = store.Load();
      saved.settings.close_to_tray = false;
      saved.settings.language = "zh-CN";
      saved.settings.selected_receiver_id = "AA:BB:CC:DD:EE:FF";
      Win2Air::ReceiverRecord receiver;
      receiver.receiver_id = saved.settings.selected_receiver_id;
      receiver.name = "Virtual receiver";
      receiver.model = "Virtual";
      saved.receivers = {receiver};
      ++saved.settings.revision;
      store.Save(saved);
    } else if (argc == 3 && std::wstring(argv[1]) == L"--activate")
      activate(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--close-playback-early")
      run_executable(argv[2], {}, true);
    else if (argc == 3 && std::wstring(argv[1]) == L"--smoke-executable")
      run_executable(argv[2], {});
    else if (argc == 3 && std::wstring(argv[1]) == L"--statistics-executable")
      run_executable(argv[2], {}, false, false, true);
    else if (argc == 3 && std::wstring(argv[1]) == L"--about-executable")
      run_executable(argv[2], {}, false, false, false, true);
    else if (argc == 3 && std::wstring(argv[1]) == L"--tray-executable")
      run_executable(argv[2], {}, false, false, false, false, true);
    else if (argc == 4 && std::wstring(argv[1]) == L"--theme-tray-executable") {
      std::string theme = winrt::to_string(argv[3]);
      if (theme != "dark" && theme != "light")
        throw std::runtime_error("Theme validation requires dark or light");
      auto root = std::filesystem::path(argv[2]).parent_path();
      Win2Air::SettingsStore store(root / L"gui-state" / L"playback-smoke");
      auto saved = store.Load();
      saved.settings.theme = theme;
      ++saved.settings.revision;
      store.Save(saved);
      expected_caption_dark = theme == "dark";
      run_executable(argv[2], {}, false, false, false, false, true);
    } else if (argc == 4 && std::wstring(argv[1]) == L"--playback-executable")
      run_executable(argv[2], argv[3]);
    else if (argc == 4 && std::wstring(argv[1]) == L"--gui-flow-executable")
      run_executable(argv[2], argv[3], false, true);
    else if (argc == 4 && std::wstring(argv[1]) == L"--store-screenshots")
      run_store_screenshots(argv[2], argv[3]);
    else if (argc == 4 && std::wstring(argv[1]) == L"--playback-window") {
      require_visible_desktop();
      auto report = std::filesystem::path(argv[3]);
      report.replace_extension(L".json");
      activate(argv[2], L"--validate-playback-ui " + report.wstring(), 15,
               argv[3]);
    } else if (argc == 3 && std::wstring(argv[1]) == L"--capture-app-window")
      capture_app_window(argv[2]);
    else if (argc == 4 &&
             std::wstring(argv[1]) == L"--capture-executable-window")
      capture_app_window(argv[3], nullptr, argv[2]);
    else if (argc == 4 && std::wstring(argv[1]) == L"--activate-validation")
      activate(argv[2], L"--validate-system-audio " + std::wstring(argv[3]),
               75);
    else if (argc == 3 && std::wstring(argv[1]) == L"--sender-abi")
      TestSenderAbi(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--settings-credentials")
      TestSettingsCredentials(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--command-lifecycle")
      TestCommandLifecycle(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--logger")
      TestLogger(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--borrowed-callbacks")
      TestBorrowedCallbacks(argv[2]);
    else if (argc == 2 && std::wstring(argv[1]) == L"--capture-source")
      TestCaptureSource();
    else if (argc == 2 && std::wstring(argv[1]) == L"--store-preview")
      TestStorePreview();
    else if (argc == 2 && std::wstring(argv[1]) == L"--audio-conversion")
      TestAudioConversion();
    else if (argc == 3 && std::wstring(argv[1]) == L"--local-playback-lease")
      TestLocalPlaybackLease(argv[2]);
    else if (argc == 2 && std::wstring(argv[1]) == L"--receiver-resolution")
      TestReceiverResolution();
    else if (argc == 4 && std::wstring(argv[1]) == L"--discover-receivers" &&
             std::wstring(argv[2]) == L"--allow-hardware")
      DiscoverReceivers(argv[3]);
    else if (argc == 4 && std::wstring(argv[1]) == L"--system-audio-session")
      TestSystemAudioSession(argv[2], argv[3]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--receiver-controls")
      TestReceiverControls(argv[2]);
    else if (argc == 2 && std::wstring(argv[1]) == L"--application-activation")
      TestApplicationActivation();
    else if (argc == 2 && std::wstring(argv[1]) == L"--tray-lifecycle")
      TestTrayLifecycle();
    else if (argc == 3 && std::wstring(argv[1]) == L"--startup-restore")
      TestStartupRestore(argv[2]);
    else if (argc == 2 && std::wstring(argv[1]) == L"--silence-standby")
      TestSilenceStandby();
    else if (argc == 3 && std::wstring(argv[1]) == L"--presentation-standby")
      TestPresentationStandby(argv[2]);
    else if (argc == 2 && std::wstring(argv[1]) == L"--reconnect-backoff")
      TestReconnectBackoff();
    else if (argc == 3 && std::wstring(argv[1]) == L"--recovery-faults")
      TestRecoveryFaults(argv[2]);
    else if (argc == 2 && std::wstring(argv[1]) == L"--adaptive-latency")
      TestAdaptiveLatency();
    else if (argc == 2 && std::wstring(argv[1]) == L"--metrics-health")
      TestMetricsHealth();
    else if (argc == 3 && std::wstring(argv[1]) == L"--diagnostics-secrets")
      TestDiagnosticsSecrets(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--metrics-diagnostics")
      TestMetricsDiagnostics(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--settings-roundtrip")
      TestSettingsRoundtrip(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--forget-pairing")
      TestForgetPairing(argv[2]);
    else if (argc == 3 && std::wstring(argv[1]) == L"--settings-accessibility")
      TestSettingsAccessibility(argv[2]);
    else if (argc == 2 &&
             std::wstring(argv[1]) == L"--presentation-measurement")
      TestPresentationMeasurement();
    else if (argc == 10 && std::wstring(argv[1]) == L"--measure-presentation")
      return MeasurePresentation(argv + 2);
    else if (argc == 2 && std::wstring(argv[1]) == L"--check-host") {
      static_assert(sizeof(void *) == 8);
      UINT32 length{};
      if (GetCurrentPackageFullName(&length, nullptr) !=
          APPMODEL_ERROR_NO_PACKAGE)
        throw std::runtime_error(
            "Offline console tests must run outside the App package.");
      std::cout << "Native x64 offline test host is ready; receiver playback "
                   "was not requested.\n";
    } else if (argc == 2 && std::wstring(argv[1]) == L"--session-state") {
      std::cout << "locked=" << session_locked() << '\n';
    } else
      throw std::runtime_error("Unknown or incomplete native test command.");
    CoUninitialize();
    return 0;
  } catch (winrt::hresult_error const &error) {
    std::cerr << winrt::to_string(error.message())
              << "; HRESULT=" << error.code().value << '\n';
    return 1;
  } catch (std::exception const &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
