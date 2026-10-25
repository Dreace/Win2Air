#include "TrayService.h"
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <stdexcept>
#include "../../resources/resource.h"
#include "../UI/ViewModels/ReceiverViewModel.h"
namespace Win2Air {
namespace {
class SystemTray final : public TrayBackend {
  HWND window_{};
  UINT restarted_{};
  std::function<void(TrayEvent, std::optional<TrayPointer>)> notify_;
  NOTIFYICONDATAW icon_{};
  HICON connected_icon_{}, disconnected_icon_{};
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam, UINT_PTR,
                                     DWORD_PTR context) noexcept {
    auto& self = *reinterpret_cast<SystemTray*>(context);
    try {
      if (message == self.restarted_) {
        self.notify_(TrayEvent::shell_restarted, std::nullopt);
      } else if (message == kTrayMessage) {
        auto event = LOWORD(lparam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT ||
            event == WM_LBUTTONDBLCLK) {
          self.notify_(TrayEvent::open, std::nullopt);
        } else if (event == WM_CONTEXTMENU) {
          TrayPointer pointer{
              static_cast<int>(static_cast<short>(LOWORD(wparam))),
              static_cast<int>(static_cast<short>(HIWORD(wparam)))};
          self.notify_(TrayEvent::activate, pointer);
        }
      }
    } catch (std::exception const& error) {
      OutputDebugStringA(error.what());
    } catch (winrt::hresult_error const& error) {
      OutputDebugStringW(error.message().c_str());
    } catch (...) {
      OutputDebugStringW(L"Notification handler failed unexpectedly");
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
  }

 public:
  SystemTray(HWND window,
             std::function<void(TrayEvent, std::optional<TrayPointer>)> notify)
      : window_(window), notify_(std::move(notify)) {
    if (!IsWindow(window_) || !notify_)
      throw std::runtime_error(
          "Tray requires an owned window and event handler");
    auto dpi = GetDpiForWindow(window_);
    auto width = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    auto height = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    // Shared resource handles live with the module and must not be destroyed.
    auto load = [&](int resource) {
      auto icon = static_cast<HICON>(
          LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resource),
                     IMAGE_ICON, width, height, LR_SHARED));
      if (!icon)
        throw std::runtime_error("Load notification icon failed: " +
                                 std::to_string(GetLastError()));
      return icon;
    };
    connected_icon_ = load(IDI_WIN2AIR);
    disconnected_icon_ = load(IDI_WIN2AIR_DISCONNECTED);
    restarted_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!restarted_ || !SetWindowSubclass(window_, WindowProc, kTrayMessage,
                                          reinterpret_cast<DWORD_PTR>(this)))
      throw std::runtime_error("Install tray window handler failed: " +
                               std::to_string(GetLastError()));
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    icon_.uCallbackMessage = kTrayMessage;
    icon_.hIcon = disconnected_icon_;
    wcscpy_s(icon_.szTip, L"Win2Air");
  }
  ~SystemTray() override {
    if (IsWindow(window_))
      RemoveWindowSubclass(window_, WindowProc, kTrayMessage);
  }
  void Add() override {
    if (!Shell_NotifyIconW(NIM_ADD, &icon_))
      throw std::runtime_error("Add notification icon failed: " +
                               std::to_string(GetLastError()));
    icon_.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &icon_)) {
      Shell_NotifyIconW(NIM_DELETE, &icon_);
      throw std::runtime_error("Set notification icon version failed");
    }
  }
  void Remove() override {
    if (!Shell_NotifyIconW(NIM_DELETE, &icon_))
      throw std::runtime_error("Remove notification icon failed");
  }
  void SetConnected(bool connected) override {
    auto next = connected ? connected_icon_ : disconnected_icon_;
    if (icon_.hIcon == next) return;
    auto update = icon_;
    update.hIcon = next;
    if (!Shell_NotifyIconW(NIM_MODIFY, &update))
      throw std::runtime_error("Update notification icon failed: " +
                               std::to_string(GetLastError()));
    icon_.hIcon = next;
  }
  void SetTooltip(std::wstring const& text) override {
    auto next = text.substr(0, std::size(icon_.szTip) - 1);
    if (next == icon_.szTip) return;
    auto update = icon_;
    wcscpy_s(update.szTip, next.c_str());
    update.uFlags = NIF_TIP | NIF_SHOWTIP;
    if (!Shell_NotifyIconW(NIM_MODIFY, &update))
      throw std::runtime_error("Update tray tooltip failed: " +
                               std::to_string(GetLastError()));
    wcscpy_s(icon_.szTip, next.c_str());
  }
  unsigned Menu(std::vector<TrayMenuItem> const& items) override {
    auto menu = CreatePopupMenu();
    if (!menu) throw std::runtime_error("Create notification menu failed");
    try {
      for (auto const& item : items)
        if (!AppendMenuW(menu,
                         MF_STRING | (item.enabled ? MF_ENABLED : MF_GRAYED) |
                             (item.checked ? MF_CHECKED : MF_UNCHECKED),
                         item.id, item.label.c_str()))
          throw std::runtime_error("Append notification menu item failed");
      POINT point{};
      if (!GetCursorPos(&point))
        throw std::runtime_error("Read notification menu position failed");
      SetForegroundWindow(window_);
      auto chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                   point.x, point.y, 0, window_, nullptr);
      PostMessageW(window_, WM_NULL, 0, 0);
      DestroyMenu(menu);
      return chosen;
    } catch (...) {
      DestroyMenu(menu);
      throw;
    }
  }
};
}  // namespace
std::vector<TrayMenuItem> TrayMenu(PlaybackSnapshot const& snapshot,
                                   TrayLabels const& labels) {
  std::vector<TrayMenuItem> items{
      {1, {TrayAction::open, {}}, labels.open},
      {2, {TrayAction::settings, {}}, labels.settings},
      {3,
       {TrayAction::disconnect, {}},
       labels.disconnect,
       snapshot.session && snapshot.session->session.has_value()}};
  auto views = ReceiverViews(snapshot, true);
  unsigned id = 100;
  for (auto const& view : views) {
    if (view.receiver.hidden && !view.active) continue;
    auto label = view.receiver.name;
    auto detail = ReceiverDetail(view.receiver);
    if (!detail.empty()) label += " | " + detail;
    items.push_back({id++,
                     {TrayAction::connect, view.receiver.receiver_id},
                     winrt::to_hstring(label).c_str(),
                     true,
                     view.active});
  }
  items.push_back({4, {TrayAction::exit, {}}, labels.exit});
  return items;
}
TrayService::TrayService(std::unique_ptr<TrayBackend> backend,
                         std::function<void(TrayCommand const&)> dispatch)
    : backend_(std::move(backend)), dispatch_(std::move(dispatch)) {
  if (!backend_ || !dispatch_)
    throw std::runtime_error(
        "Tray service requires backend and command dispatch");
}
TrayService::~TrayService() {
  try {
    Stop();
  } catch (...) {
  }
}
void TrayService::Start() {
  if (!added_) {
    backend_->Add();
    added_ = true;
  }
}
void TrayService::Stop() {
  if (added_) {
    backend_->Remove();
    added_ = false;
  }
}
void TrayService::Update(PlaybackSnapshot const& snapshot) {
  if (!added_) return;
  bool connected =
      snapshot.session && snapshot.session->session &&
      (snapshot.session->session->state == SessionState::ready ||
       snapshot.session->session->state == SessionState::streaming);
  std::wstring tooltip = L"Win2Air";
  if (connected) {
    auto const& identity = snapshot.session->session->receiver_id;
    for (auto const& receiver : snapshot.receivers)
      if (receiver.receiver_id == identity) {
        tooltip += L"\n";
        tooltip += winrt::to_hstring(receiver.name).c_str();
        tooltip += L"\n";
        tooltip += winrt::to_hstring(ReceiverDetail(receiver)).c_str();
        break;
      }
  }
  backend_->SetTooltip(tooltip);
  if (connected_ != connected) {
    backend_->SetConnected(connected);
    connected_ = connected;
  }
}
void TrayService::Handle(TrayEvent event, PlaybackSnapshot const& snapshot,
                         TrayLabels const& labels) {
  if (event == TrayEvent::shell_restarted) {
    added_ = false;
    Start();
    Update(snapshot);
    return;
  }
  if (!added_) throw std::runtime_error("Notification icon is unavailable");
  if (event == TrayEvent::open) {
    dispatch_({TrayAction::open, {}});
    return;
  }
  if (event == TrayEvent::activate) {
    dispatch_({TrayAction::flyout, {}});
    return;
  }
  auto items = TrayMenu(snapshot, labels);
  auto selected = backend_->Menu(items);
  if (!selected) return;
  auto found = std::find_if(items.begin(), items.end(), [&](auto const& item) {
    return item.id == selected;
  });
  if (found == items.end() || !found->enabled)
    throw std::runtime_error("Notification menu action is unavailable");
  dispatch_(found->command);
}
std::unique_ptr<TrayBackend> NativeTrayBackend(
    HWND window,
    std::function<void(TrayEvent, std::optional<TrayPointer>)> notify) {
  return std::make_unique<SystemTray>(window, std::move(notify));
}
TrayArea PlaceTrayFlyout(TrayArea const& work, int pointer_x, int pointer_y,
                         int width, int height) {
  if (work.width <= 0 || work.height <= 0 || width <= 0 || height <= 0)
    throw std::invalid_argument(
        "Tray flyout requires a positive window and work area: " +
        std::to_string(width) + "x" + std::to_string(height));
  if (width > work.width) width = work.width;
  if (height > work.height) height = work.height;
  auto left = pointer_x;
  auto top = pointer_y;
  if (left + width > work.x + work.width) left = pointer_x - width;
  if (top + height > work.y + work.height) top = pointer_y - height;
  if (left < work.x) left = work.x;
  if (top < work.y) top = work.y;
  if (left + width > work.x + work.width) left = work.x + work.width - width;
  if (top + height > work.y + work.height) top = work.y + work.height - height;
  return {left, top, width, height};
}
TrayFlyoutAction TrayFlyoutClick(
    bool visible,
    std::optional<std::chrono::steady_clock::time_point> hidden_at,
    std::chrono::steady_clock::time_point now) {
  if (visible) return TrayFlyoutAction::hide;
  if (hidden_at && now < *hidden_at)
    throw std::invalid_argument(
        "Tray click is earlier than the flyout dismissal");
  if (hidden_at && now - *hidden_at < kTrayFlyoutToggle)
    return TrayFlyoutAction::keep_closed;
  return TrayFlyoutAction::show;
}
CloseAction WindowClose(bool close_to_tray, bool icon_visible) {
  return close_to_tray && icon_visible ? CloseAction::hide : CloseAction::exit;
}
}  // namespace Win2Air
