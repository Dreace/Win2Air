#pragma once
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <netioapi.h>
#include <functional>
#include <optional>
#include "../Streaming/SessionModels.h"
namespace Win2Air {
std::optional<SystemEvent> PowerEvent(WPARAM event) noexcept;
class PowerNotifications {
  HWND window_{};
  HANDLE network_{};
  std::function<void(SystemEvent, unsigned)> notify_;
  static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                     DWORD_PTR) noexcept;
  static void CALLBACK Network(void*, MIB_IPINTERFACE_ROW*,
                               MIB_NOTIFICATION_TYPE) noexcept;

 public:
  PowerNotifications(HWND, std::function<void(SystemEvent, unsigned)>);
  ~PowerNotifications();
  PowerNotifications(PowerNotifications const&) = delete;
  PowerNotifications& operator=(PowerNotifications const&) = delete;
};
}  // namespace Win2Air
