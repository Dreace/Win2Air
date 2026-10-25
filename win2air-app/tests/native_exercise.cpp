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

using Microsoft::WRL::ComPtr;

#include "native_gui.h"

bool exercise_shell(HWND window, unsigned &step,
                    std::filesystem::path const &screenshot,
                    std::filesystem::path const &executable) {
  ComPtr<IUIAutomation> automation;
  ComPtr<IUIAutomationElement> root;
  check(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&automation)),
        "Create GUI flow automation");
  check(automation->ElementFromHandle(window, &root), "Find GUI flow root");
  auto find = [&](wchar_t const *name) {
    ComPtr<IUIAutomationCondition> condition;
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(name);
    auto code = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,
                                                    value, &condition);
    VariantClear(&value);
    check(code, "Create GUI control condition");
    ComPtr<IUIAutomationElement> element;
    check(root->FindFirst(TreeScope_Descendants, condition.Get(), &element),
          "Find GUI control");
    return element;
  };
  wchar_t const *ready[] = {L"Connect",        L"Connect", L"ExportDiagnostics",
                            L"StartupEnabled", L"Connect", L"StartupEnabled",
                            L"ShowHidden"};
  if (!find(ready[step])) return false;
  if (step == 0) {
    auto slider = find(L"DestinationVolume");
    if (!slider) {
      if (find(L"CurrentDisconnect")) return false;
      // Inactive playback omits volume controls from the automation tree.
      ++step;
      return false;
    }
    BOOL adjustable{};
    check(slider->get_CurrentIsEnabled(&adjustable),
          "Read volume availability");
    if (!adjustable) {
      // The offline fixture has no active stream. A connected virtual receiver
      // exercises the continuous-drag branch below.
      ++step;
      return false;
    }
    RECT bounds{};
    check(slider->get_CurrentBoundingRectangle(&bounds),
          "Locate volume slider");
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
      return false;
    ComPtr<IUIAutomationRangeValuePattern> range;
    check(slider->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                      IID_PPV_ARGS(&range)),
          "Read volume range");
    check(range->SetValue(50), "Set the starting volume");
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    SetForegroundWindow(window);
    std::cout << "Volume slider bounds=" << bounds.left << ',' << bounds.top
              << ',' << bounds.right << ',' << bounds.bottom
              << " foreground=" << (GetForegroundWindow() == window) << '\n';
    auto drag_capture =
        screenshot.parent_path() / (screenshot.stem().wstring() + L"-drag.png");
    capture_app_window(drag_capture, window, executable);
    {
      POINT original{};
      GetPhysicalCursorPos(&original);
      struct ReleaseMouse {
        POINT original;
        ~ReleaseMouse() {
          INPUT release{};
          release.type = INPUT_MOUSE;
          release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
          SendInput(1, &release, sizeof(release));
          SetPhysicalCursorPos(original.x, original.y);
        }
      } release{original};
      auto move = [&](double fraction) {
        if (!SetPhysicalCursorPos(
                bounds.left +
                    static_cast<int>((bounds.right - bounds.left) * fraction),
                (bounds.top + bounds.bottom) / 2))
          winrt::throw_last_error();
      };
      move(0.5);
      INPUT down{};
      down.type = INPUT_MOUSE;
      down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
      if (SendInput(1, &down, sizeof(down)) != 1) winrt::throw_last_error();
      for (unsigned position = 51; position <= 85; ++position) {
        move(double(position) / 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        BOOL enabled{};
        check(slider->get_CurrentIsEnabled(&enabled),
              "Read dragging availability");
        if (!enabled)
          throw std::runtime_error(
              "Volume slider became disabled during dragging");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    double value{};
    check(range->get_CurrentValue(&value), "Read dragged volume");
    if (value < 75 || value > 95)
      throw std::runtime_error(
          "Volume thumb stopped following the pointer: value=" +
          std::to_string(value));
    std::cout << "Continuous mouse drag kept the volume slider active.\n";
    ++step;
    return false;
  }
  if (step == 6) return true;
  if (step == 2 && !screenshot.empty()) {
    auto statistics = screenshot.parent_path() /
                      (screenshot.stem().wstring() + L"-statistics.png");
    capture_app_window(statistics, window, executable);
  }
  wchar_t const *destinations[] = {L"StatisticsNavigation",
                                   L"SettingsNavigation", L"PlaybackNavigation",
                                   L"SettingsNavigation", L"ReceiversCategory"};
  auto item = find(destinations[step - 1]);
  if (!item) return false;
  ComPtr<IUIAutomationSelectionItemPattern> selection;
  check(item->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                  IID_PPV_ARGS(&selection)),
        "Read GUI navigation pattern");
  check(selection->Select(), "Navigate through actual GUI pages");
  ++step;
  return false;
}
bool exercise_information_page(HWND window, unsigned &step, bool about) {
  ComPtr<IUIAutomation> automation;
  ComPtr<IUIAutomationElement> root;
  check(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&automation)),
        "Create statistics automation");
  check(automation->ElementFromHandle(window, &root), "Find application root");
  auto find = [&](wchar_t const *name) {
    ComPtr<IUIAutomationCondition> condition;
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(name);
    auto result = automation->CreatePropertyCondition(
        UIA_AutomationIdPropertyId, value, &condition);
    VariantClear(&value);
    check(result, "Create statistics control condition");
    ComPtr<IUIAutomationElement> element;
    check(root->FindFirst(TreeScope_Descendants, condition.Get(), &element),
          "Find statistics control");
    return element;
  };
  if (!step || (about && step == 1)) {
    auto item =
        find(about ? (step == 0 ? L"SettingsNavigation" : L"AboutCategory")
                   : L"StatisticsNavigation");
    if (!item) return false;
    ComPtr<IUIAutomationSelectionItemPattern> selection;
    check(item->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                    IID_PPV_ARGS(&selection)),
          "Read statistics navigation action");
    check(selection->Select(), "Open compiled statistics page");
    ++step;
    return false;
  }
  if (!about) return find(L"HistoryMetric") && find(L"AudioHistoryMetric");
  for (auto id : {L"OpenSourceProject_Win2Air", L"OpenSourceProject_OwnTone",
                  L"OpenSourceLicense_OwnTone", L"AboutRepository"}) {
    auto link = find(id);
    if (!link) return false;
    ComPtr<IUIAutomationInvokePattern> invoke;
    check(link->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke)),
          "Open source project and license expose accessible link actions");
  }
  return true;
}
std::optional<bool> expected_caption_dark;
static std::chrono::steady_clock::time_point tray_click_time;
bool exercise_tray(HWND window, unsigned &step) {
  if (!step) {
    tray_click_time = std::chrono::steady_clock::now();
    PostMessageW(window, Win2Air::kTrayMessage, 0, WM_CONTEXTMENU);
    ++step;
    return false;
  }
  ComPtr<IUIAutomation> automation;
  ComPtr<IUIAutomationElement> desktop;
  check(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&automation)),
        "Create tray automation");
  struct FlyoutSearch {
    DWORD process;
    HWND main;
    HWND found{};
  } search{};
  GetWindowThreadProcessId(window, &search.process);
  search.main = window;
  EnumWindows(
      [](HWND candidate, LPARAM context) -> BOOL {
        auto &state = *reinterpret_cast<FlyoutSearch *>(context);
        DWORD process{};
        GetWindowThreadProcessId(candidate, &process);
        if (process == state.process && candidate != state.main &&
            IsWindowVisible(candidate))
          state.found = candidate;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  if (!search.found) return false;
  check(automation->ElementFromHandle(search.found, &desktop),
        "Read tray automation root");
  DWORD process{};
  GetWindowThreadProcessId(window, &process);
  VARIANT value{};
  value.vt = VT_I4;
  value.lVal = process;
  ComPtr<IUIAutomationCondition> owner, control, condition;
  check(automation->CreatePropertyCondition(UIA_ProcessIdPropertyId, value,
                                            &owner),
        "Match application process");
  value.vt = VT_BSTR;
  value.bstrVal = SysAllocString(L"TrayOpen");
  auto result = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,
                                                    value, &control);
  VariantClear(&value);
  check(result, "Match tray open action");
  check(automation->CreateAndCondition(owner.Get(), control.Get(), &condition),
        "Match owned tray action");
  ComPtr<IUIAutomationElement> button;
  check(desktop->FindFirst(TreeScope_Descendants, condition.Get(), &button),
        "Find loaded tray flyout");
  if (!button) return false;
  BOOL offscreen{};
  check(button->get_CurrentIsOffscreen(&offscreen),
        "Read tray presentation readiness");
  if (offscreen) return false;
  if (step == 1) {
    std::cout << "Tray right-click activation: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - tray_click_time)
                     .count()
              << " ms\n";
    if (expected_caption_dark) {
      BOOL dark{};
      check(DwmGetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                                  sizeof(dark)),
            "Read actual native title-bar theme");
      if ((dark != FALSE) != *expected_caption_dark)
        throw std::runtime_error(
            "Native title-bar mode must follow the selected app theme");
      COLORREF color{};
      auto caption_result = DwmGetWindowAttribute(window, DWMWA_CAPTION_COLOR,
                                                  &color, sizeof(color));
      if (caption_result != E_INVALIDARG) {
        check(caption_result, "Read native caption color");
        bool dark_color = (unsigned(GetRValue(color)) + GetGValue(color) +
                           GetBValue(color)) < 384;
        if (dark_color != *expected_caption_dark)
          throw std::runtime_error(
              "Native caption color must follow the selected app theme");
      }
    }
  }
  ComPtr<IUIAutomationInvokePattern> invoke;
  check(button->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke)),
        "Read flyout open command");
  check(invoke->Invoke(), "Open main window from flyout");
  if (step == 1) {
    Sleep(static_cast<DWORD>(Win2Air::kTrayFlyoutToggle.count() + 1));
    PostMessageW(window, Win2Air::kTrayMessage, 0, WM_CONTEXTMENU);
    ++step;
    return false;
  }
  ShowWindow(window, SW_MINIMIZE);
  SendMessageW(window, Win2Air::kTrayMessage, 0, NIN_SELECT);
  if (IsIconic(window) || !IsWindowVisible(window) ||
      GetForegroundWindow() != window)
    throw std::runtime_error(
        "Tray left click must immediately restore and foreground the main "
        "window");
  return true;
}
