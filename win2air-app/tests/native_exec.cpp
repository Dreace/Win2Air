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
using Microsoft::WRL::ComPtr;

#include "native_gui.h"

void run_executable(std::filesystem::path const &executable,
                    std::filesystem::path const &screenshot, bool close_early,
                    bool full_flow, bool statistics, bool about, bool tray) {
  if (!screenshot.empty()) require_visible_desktop();
  if (!executable.is_absolute() ||
      (!screenshot.empty() && !screenshot.is_absolute()) ||
      executable.filename() != L"Win2Air.exe")
    throw std::runtime_error(
        "Direct UI validation requires absolute App and screenshot paths");
  auto report = screenshot.empty()
                    ? executable.parent_path() / L"playback-smoke.json"
                    : screenshot;
  report.replace_extension(L".json");
  std::filesystem::remove(report);
  auto arguments = L"\"" + executable.wstring() +
                   L"\" --validate-playback-ui " + report.wstring();
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr,
                      FALSE, 0, nullptr, executable.parent_path().c_str(),
                      &startup, &child))
    throw std::runtime_error("Start compiled App failed: " +
                             std::to_string(GetLastError()));
  winrt::handle process{child.hProcess}, thread{child.hThread};
  WindowSearch search{child.dwProcessId, false};
  std::exception_ptr capture_error;
  bool captured = screenshot.empty();
  bool close_sent = false;
  unsigned navigation_step{};
  bool navigated = !full_flow && !statistics && !about && !tray;
  auto began = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - began < std::chrono::seconds(15)) {
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    if (full_flow && !navigated && search.window)
      navigated = exercise_shell(search.window, navigation_step, screenshot,
                                 executable);
    if ((statistics || about) && !navigated && search.window)
      navigated =
          exercise_information_page(search.window, navigation_step, about);
    if (tray && !navigated && search.window)
      navigated = exercise_tray(search.window, navigation_step);
    if (close_early && !close_sent && search.window) {
      if (!PostMessageW(search.window, WM_CLOSE, 0, 0))
        winrt::throw_last_error();
      close_sent = true;
    }
    if (!captured && navigated && search.window &&
        std::chrono::steady_clock::now() - began >= std::chrono::seconds(4)) {
      try {
        capture_app_window(screenshot, search.window, executable);
      } catch (...) {
        capture_error = std::current_exception();
      }
      captured = true;
    }
    if (WaitForSingleObject(process.get(), 50) == WAIT_OBJECT_0) break;
  }
  DWORD code{};
  if (!GetExitCodeProcess(process.get(), &code)) winrt::throw_last_error();
  if (code == STILL_ACTIVE) {
    TerminateProcess(process.get(), ERROR_TIMEOUT);
    WaitForSingleObject(process.get(), 3000);
  }
  if (capture_error) std::rethrow_exception(capture_error);
  if (code || !captured || !search.visible || !navigated)
    throw std::runtime_error("Compiled App UI validation failed: exit=" +
                             std::to_string(code));
  std::ifstream report_file(report);
  std::string contents((std::istreambuf_iterator<char>(report_file)), {});
  auto state = winrt::Windows::Data::Json::JsonObject::Parse(
      winrt::to_hstring(contents));
  if (!state.GetNamedBoolean(L"loaded"))
    throw std::runtime_error("Compiled playback page did not finish loading");
  std::cout << (about ? "Compiled App opened About with accessible project and "
                        "license links"
                : statistics ? "Compiled App loaded both statistics charts"
                             : "Compiled App created its playback window")
            << " and exited without installation.\n";
}
void activate(std::wstring const &aumid, std::wstring const &arguments,
              unsigned timeout, std::filesystem::path const &screenshot) {
  ComPtr<IApplicationActivationManager> manager;
  check(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                         CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)),
        "Create packaged activation manager");
  DWORD process_id{};
  check(manager->ActivateApplication(aumid.c_str(), arguments.c_str(),
                                     AO_NOERRORUI, &process_id),
        "Activate installed Win2Air");
  HANDLE process = OpenProcess(
      SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
      FALSE, process_id);
  if (!process)
    throw std::runtime_error("Open activated process failed: " +
                             std::to_string(GetLastError()));
  wchar_t image[32768]{};
  DWORD image_size = 32768;
  if (!QueryFullProcessImageNameW(process, 0, image, &image_size)) {
    CloseHandle(process);
    throw std::runtime_error("Read actual packaged executable identity failed");
  }
  std::wcout << L"Actual App executable: " << image << L'\n';
  WindowSearch search{process_id, false};
  bool packaged = false;
  bool captured = false;
  std::exception_ptr capture_error;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
  auto capture_after =
      std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < deadline) {
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
    if (!screenshot.empty() && !captured && search.window &&
        std::chrono::steady_clock::now() >= capture_after) {
      try {
        capture_app_window(screenshot, search.window);
      } catch (...) {
        capture_error = std::current_exception();
      }
      captured = true;
    }
    UINT32 length = 0;
    if (GetPackageFullName(process, &length, nullptr) ==
        ERROR_INSUFFICIENT_BUFFER) {
      std::wstring name(length, L'\0');
      if (GetPackageFullName(process, &length, name.data()) == ERROR_SUCCESS)
        packaged = name.starts_with(L"Win2Air.Desktop_") &&
                   name.find(L"_x64__") != std::wstring::npos;
    }
    if (WaitForSingleObject(process, 50) == WAIT_OBJECT_0) break;
  }
  DWORD code = STILL_ACTIVE;
  GetExitCodeProcess(process, &code);
  if (code == STILL_ACTIVE) {
    if (search.window) {
      ComPtr<IUIAutomation> automation;
      ComPtr<IUIAutomationElement> root;
      ComPtr<IUIAutomationCondition> condition;
      ComPtr<IUIAutomationElementArray> elements;
      if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&automation))) &&
          SUCCEEDED(automation->ElementFromHandle(search.window, &root)) &&
          SUCCEEDED(automation->CreateTrueCondition(&condition)) &&
          SUCCEEDED(root->FindAll(TreeScope_Descendants, condition.Get(),
                                  &elements))) {
        int count{};
        elements->get_Length(&count);
        for (int i = 0; i < count && i < 128; ++i) {
          ComPtr<IUIAutomationElement> element;
          BSTR text{};
          if (SUCCEEDED(elements->GetElement(i, &element)) &&
              SUCCEEDED(element->get_CurrentName(&text)) && text) {
            std::wcerr << L"Window accessibility text: " << text << L'\n';
            SysFreeString(text);
          }
        }
      }
    }
    TerminateProcess(process, ERROR_TIMEOUT);
    WaitForSingleObject(process, 3000);
  }
  CloseHandle(process);
  if (capture_error) std::rethrow_exception(capture_error);
  if (!search.visible || !packaged || code != 0)
    throw std::runtime_error("Packaged window smoke failed: visible=" +
                             std::to_string(search.visible) +
                             " packaged=" + std::to_string(packaged) +
                             " exit=" + std::to_string(code));
  std::cout << "Installed x64 package created a visible WinUI window and "
               "exited within the deadline.\n";
}
void publish_media_fixture(std::filesystem::path const &artwork) {
  using namespace winrt::Windows::Media;
  using namespace winrt::Windows::Storage;
  using namespace winrt::Windows::Storage::Streams;
  if (!artwork.is_absolute() || !std::filesystem::is_regular_file(artwork))
    throw std::runtime_error("Media fixture requires an absolute image path");
  auto thumbnail =
      std::async(std::launch::async, [artwork] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto native_path = artwork;
        auto file = StorageFile::GetFileFromPathAsync(
                        native_path.make_preferred().wstring())
                        .get();
        auto reference = RandomAccessStreamReference::CreateFromFile(file);
        file = nullptr;
        winrt::uninit_apartment();
        return reference;
      }).get();
  WNDCLASSW type{};
  type.lpfnWndProc = DefWindowProcW;
  type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"Win2AirMediaFixture";
  if (!RegisterClassW(&type)) winrt::throw_last_error();
  HWND window = CreateWindowW(type.lpszClassName, L"Win2Air media fixture",
                              WS_OVERLAPPEDWINDOW, 0, 0, 320, 200, nullptr,
                              nullptr, type.hInstance, nullptr);
  if (!window) winrt::throw_last_error();
  auto close =
      std::unique_ptr<std::remove_pointer_t<HWND>, decltype(&DestroyWindow)>(
          window, DestroyWindow);
  auto factory =
      winrt::get_activation_factory<SystemMediaTransportControls,
                                    ISystemMediaTransportControlsInterop>();
  SystemMediaTransportControls controls{nullptr};
  check(factory->GetForWindow(window,
                              winrt::guid_of<SystemMediaTransportControls>(),
                              winrt::put_abi(controls)),
        "Create virtual Windows media session");
  Win2Air::SystemMediaSession reader;
  controls.IsEnabled(true);
  controls.IsPlayEnabled(true);
  controls.PlaybackStatus(MediaPlaybackStatus::Playing);
  auto updater = controls.DisplayUpdater();
  updater.Type(MediaPlaybackType::Music);
  updater.MusicProperties().Title(L"Windows media fixture");
  updater.MusicProperties().Artist(L"Virtual artist");
  updater.MusicProperties().AlbumTitle(L"Virtual album");
  updater.Thumbnail(thumbnail);
  updater.Update();
  bool observed = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
  while (std::chrono::steady_clock::now() < deadline) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    auto media = reader.Snapshot();
    if (!observed && media && media->title == "Windows media fixture" &&
        media->album == "Virtual album" && !media->artwork.empty()) {
      observed = true;
      std::cout << "Windows system session title, album and artwork observed\n"
                << std::flush;
    }
    Sleep(20);
  }
  controls.PlaybackStatus(MediaPlaybackStatus::Closed);
  controls.IsEnabled(false);
  if (!observed)
    throw std::runtime_error(
        "The virtual Windows media session was not observed");
}
void require_store_png(std::filesystem::path const &path) {
  std::ifstream file(path, std::ios::binary);
  unsigned char header[24]{};
  file.read(reinterpret_cast<char *>(header), 24);
  auto quad = [](unsigned char const *bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | bytes[3];
  };
  auto width = quad(header + 16);
  auto height = quad(header + 20);
  if (!file ||
      std::string(reinterpret_cast<char *>(header), 8) !=
          std::string("\x89PNG\r\n\x1a\n", 8) ||
      width != 1920 || height != 1080)
    throw std::runtime_error(
        "Store screenshot must be 1920x1080: " + path.string() + " is " +
        std::to_string(width) + "x" + std::to_string(height));
}
void run_store_screenshots(std::filesystem::path const &executable,
                           std::filesystem::path const &output) {
  TestStorePreview();
  require_visible_desktop();
  if (!executable.is_absolute() || !output.is_absolute() ||
      executable.filename() != L"Win2Air.exe")
    throw std::runtime_error(
        "Store screenshots require absolute App and output paths");
  std::filesystem::remove_all(output);
  std::filesystem::create_directories(output);
  auto arguments = L"\"" + executable.wstring() + L"\" --store-screenshots \"" +
                   output.wstring() + L"\"";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr,
                      FALSE, 0, nullptr, executable.parent_path().c_str(),
                      &startup, &child))
    throw std::runtime_error("Start store screenshot App failed: " +
                             std::to_string(GetLastError()));
  winrt::handle process{child.hProcess}, thread{child.hThread};
  WindowSearch search{child.dwProcessId, false};
  wchar_t const *steps[] = {L"zh-CN-playback",   L"zh-CN-statistics",
                            L"zh-CN-settings",   L"en-US-playback",
                            L"en-US-statistics", L"en-US-settings"};
  auto fail_if_reported = [&] {
    auto error = output / L"error.txt";
    if (!std::filesystem::exists(error)) return;
    std::ifstream file(error);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    throw std::runtime_error("Store screenshot App failed: " + text);
  };
  for (auto step : steps) {
    std::string expected;
    for (wchar_t ch : std::wstring(step)) expected.push_back(char(ch));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
      EnumWindows(find_window, reinterpret_cast<LPARAM>(&search));
      fail_if_reported();
      auto path = output / L"ready.txt";
      if (std::filesystem::exists(path)) {
        std::ifstream file(path);
        std::string text((std::istreambuf_iterator<char>(file)), {});
        if (text == expected) {
          ready = true;
          break;
        }
      }
      if (WaitForSingleObject(process.get(), 50) == WAIT_OBJECT_0)
        throw std::runtime_error("Store screenshot App exited before " +
                                 expected);
    }
    if (!ready || !search.window)
      throw std::runtime_error("Store screenshot page was not ready: " +
                               expected);
    capture_app_window(output / (std::wstring(step) + L".png"), search.window,
                       executable);
    {
      std::ofstream ack(output / L"ack.txt",
                        std::ios::binary | std::ios::trunc);
      ack << expected;
      ack.flush();
      if (!ack)
        throw std::runtime_error(
            "Write store screenshot acknowledgement failed");
    }
  }
  if (WaitForSingleObject(process.get(), 20000) != WAIT_OBJECT_0) {
    TerminateProcess(process.get(), ERROR_TIMEOUT);
    throw std::runtime_error("Store screenshot App did not exit");
  }
  DWORD code{};
  if (!GetExitCodeProcess(process.get(), &code)) winrt::throw_last_error();
  fail_if_reported();
  if (code)
    throw std::runtime_error("Store screenshot App failed: exit=" +
                             std::to_string(code));
  for (auto step : steps)
    require_store_png(output / (std::wstring(step) + L".png"));
  std::wcout << L"Store screenshots saved to " << output.wstring() << L'\n';
}
