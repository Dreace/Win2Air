#include "PowerNotifications.h"
#include <commctrl.h>
#include <stdexcept>
namespace Win2Air {
std::optional<SystemEvent> PowerEvent(WPARAM event) noexcept {
  if (event == PBT_APMSUSPEND) return SystemEvent::suspend;
  if (event == PBT_APMRESUMEAUTOMATIC) return SystemEvent::resume;
  return std::nullopt;
}
LRESULT CALLBACK PowerNotifications::WindowProc(HWND window, UINT message,
                                                WPARAM wparam, LPARAM lparam,
                                                UINT_PTR,
                                                DWORD_PTR context) noexcept {
  auto& self = *reinterpret_cast<PowerNotifications*>(context);
  if (message == WM_POWERBROADCAST)
    if (auto event = PowerEvent(wparam)) {
      self.notify_(*event, 0);
      return TRUE;
    }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, WindowProc, 1);
    self.window_ = nullptr;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}
void CALLBACK PowerNotifications::Network(void* context,
                                          MIB_IPINTERFACE_ROW* row,
                                          MIB_NOTIFICATION_TYPE type) noexcept {
  auto& self = *static_cast<PowerNotifications*>(context);
  if (type != MibInitialNotification)
    self.notify_(SystemEvent::network_changed, row ? row->InterfaceIndex : 0);
}
PowerNotifications::PowerNotifications(
    HWND window, std::function<void(SystemEvent, unsigned)> notify)
    : window_(window), notify_(std::move(notify)) {
  if (!window || !notify_)
    throw std::invalid_argument(
        "System notifications require a window and sink");
  if (!SetWindowSubclass(window, WindowProc, 1,
                         reinterpret_cast<DWORD_PTR>(this)))
    throw std::runtime_error("Register power notifications failed: " +
                             std::to_string(GetLastError()));
  auto status =
      NotifyIpInterfaceChange(AF_UNSPEC, Network, this, FALSE, &network_);
  if (status != NO_ERROR) {
    RemoveWindowSubclass(window, WindowProc, 1);
    throw std::runtime_error("Register network notifications failed: " +
                             std::to_string(status));
  }
}
PowerNotifications::~PowerNotifications() {
  if (network_) CancelMibChangeNotify2(network_);
  if (window_) RemoveWindowSubclass(window_, WindowProc, 1);
}
}  // namespace Win2Air
