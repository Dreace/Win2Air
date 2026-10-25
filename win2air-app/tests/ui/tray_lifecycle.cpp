#include <windows.h>
#undef GetCurrentTime
#undef GetClassName
#include <shellapi.h>
#include "../../resources/resource.h"
#include "../../src/Shell/TrayService.h"
#include "../../src/UI/ViewModels/TrayViewModel.h"
#include "../contract/test_support.h"
using namespace Win2Air;
namespace {
struct Tray final : TrayBackend {
  unsigned added{}, removed{}, selected{};
  bool fail_add{}, fail_remove{};
  std::vector<bool> connection_updates, added_states;
  bool connected{};
  std::wstring tooltip;
  void SetTooltip(std::wstring const& text) override { tooltip = text; }
  void Add() override {
    if (fail_add) throw std::runtime_error("Virtual shell unavailable");
    ++added;
    added_states.push_back(connected);
  }
  void Remove() override {
    if (fail_remove) throw std::runtime_error("Virtual icon removal rejected");
    ++removed;
  }
  unsigned Menu(std::vector<TrayMenuItem> const&) override { return selected; }
  void SetConnected(bool value) override {
    connected = value;
    connection_updates.push_back(value);
  }
};
}  // namespace
void TestTrayLifecycle() {
  auto window =
      CreateWindowExW(0, L"STATIC", L"Tray event test", 0, 0, 0, 0, 0, nullptr,
                      nullptr, GetModuleHandleW(nullptr), nullptr);
  Require(window != nullptr, "Create hidden notification event test window");
  try {
    std::vector<TrayEvent> events;
    auto backend = NativeTrayBackend(
        window, [&](auto event, auto) { events.push_back(event); });
    SendMessageW(window, kTrayMessage, MAKELONG(20, 30), NIN_SELECT);
    Require(events == std::vector<TrayEvent>{TrayEvent::open},
            "Left click immediately opens the main window");
    SendMessageW(window, kTrayMessage, 0, WM_LBUTTONDBLCLK);
    Require(events.back() == TrayEvent::open && events.size() == 2,
            "Further left clicks keep opening the main window");
    SendMessageW(window, kTrayMessage, 0, NIN_KEYSELECT);
    Require(events.back() == TrayEvent::open && events.size() == 3,
            "Keyboard activation opens the main window");
    SendMessageW(window, kTrayMessage, MAKELONG(20, 30), WM_CONTEXTMENU);
    Require(events.back() == TrayEvent::activate && events.size() == 4,
            "Right click opens the tray flyout immediately");
  } catch (...) {
    DestroyWindow(window);
    throw;
  }
  DestroyWindow(window);
  for (auto resource : {IDI_WIN2AIR, IDI_WIN2AIR_DISCONNECTED}) {
    for (auto size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
      auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                                MAKEINTRESOURCEW(resource),
                                                IMAGE_ICON, size, size, 0));
      Require(icon != nullptr,
              "Application icon resources load at shell DPI sizes");
      if (size == 32) {
        ICONINFO info{};
        Require(GetIconInfo(icon, &info), "Read notification icon pixels");
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = size;
        bitmap.bmiHeader.biHeight = -size;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> pixels(size * size);
        auto dc = GetDC(nullptr);
        auto rows = GetDIBits(dc, info.hbmColor, 0, size, pixels.data(),
                              &bitmap, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        DestroyIcon(icon);
        Require(rows == size, "Decode notification icon pixels");
        bool visible{}, colored{};
        for (auto pixel : pixels) {
          if (!(pixel >> 24)) continue;
          visible = true;
          auto blue = pixel & 255;
          auto green = (pixel >> 8) & 255;
          auto red = (pixel >> 16) & 255;
          colored |= red != green || green != blue;
        }
        Require(visible && colored == (resource == IDI_WIN2AIR),
                "Connected artwork keeps its colors and disconnected artwork "
                "is gray");
        continue;
      }
      DestroyIcon(icon);
    }
  }
  auto backend = std::make_unique<Tray>();
  auto& shell = *backend;
  std::vector<TrayCommand> commands;
  TrayService service(std::move(backend), [&](auto const& command) {
    commands.push_back(command);
  });
  service.Start();
  service.Start();
  Require(shell.added == 1 && service.Visible(),
          "Repeated startup owns one virtual notification icon");
  PlaybackSnapshot snapshot;
  service.Update(snapshot);
  service.Update(snapshot);
  Require(shell.connection_updates == std::vector<bool>{false},
          "An unconnected tray is gray without repeated shell updates");
  auto session = std::make_shared<SessionSnapshot>();
  snapshot.session = session;
  ReceiverRecord current;
  current.receiver_id = "AA:BB:CC:DD:EE:FF";
  current.name = "Equal name";
  current.model = "HomePod mini";
  current.manual_endpoint =
      ReceiverEndpoint{"virtual.local", "127.0.0.1", 7000};
  current.hidden = true;
  auto other = current;
  other.receiver_id = "11:22:33:44:55:66";
  other.manual_endpoint.reset();
  other.discovered_endpoints = {
      ReceiverEndpoint{"other.local", "127.0.0.2", 7000}};
  other.hidden = false;
  snapshot.receivers = {current, other};
  session->session = ReceiverSession{};
  session->session->receiver_id = current.receiver_id;
  session->session->state = SessionState::ready;
  service.Update(snapshot);
  session->session->state = SessionState::streaming;
  Require(shell.tooltip == L"Win2Air\nEqual name\nHomePod mini | 127.0.0.1",
          "Tray tooltip shows the current receiver name, model and IP address");
  snapshot.receivers.front().manual_endpoint->resolved_address = "127.0.0.9";
  service.Update(snapshot);
  Require(
      shell.tooltip == L"Win2Air\nEqual name\nHomePod mini | 127.0.0.9",
      "Endpoint changes refresh the tooltip without a connection state change");
  snapshot.receivers.front().manual_endpoint->resolved_address = "127.0.0.1";
  service.Update(snapshot);
  Require(shell.connection_updates == std::vector<bool>({false, true}),
          "Ready and playing sessions use the color icon");
  for (auto state : {SessionState::recovering, SessionState::error,
                     SessionState::disconnected, SessionState::connecting,
                     SessionState::pairing, SessionState::setting_up,
                     SessionState::ending}) {
    session->session->state = state;
    service.Update(snapshot);
    Require(!shell.connected,
            "An unavailable receiver uses the gray tray icon");
    Require(shell.tooltip == L"Win2Air",
            "An unavailable receiver returns to the application tooltip");
  }
  session->session->state = SessionState::ready;
  service.Update(snapshot);
  session->intent.desired_connected = true;
  TrayLabels labels{L"Open", L"Settings", L"Exit", L"Disconnect"};
  auto menu = TrayMenu(snapshot, labels);
  Require(menu.size() == 6 && menu[3].checked && menu[3].label != menu[4].label,
          "Menu keeps hidden active receiver and distinguishes equal names by "
          "address");
  Require(menu[3].label == L"Equal name | HomePod mini | 127.0.0.1" &&
              menu[4].label == L"Equal name | HomePod mini | 127.0.0.2",
          "Tray menus show the same model and IP address as playback cards");
  for (auto const& entry : menu)
    Require(entry.label.find(L"AA:BB:CC:DD:EE:FF") == std::wstring::npos &&
                entry.label.find(L"11:22:33:44:55:66") == std::wstring::npos,
            "Receiver identities stay out of visible menu labels");
  shell.selected = menu[4].id;
  service.Handle(TrayEvent::menu, snapshot, labels);
  Require(
      commands.back().action == TrayAction::connect &&
          commands.back().receiver_id == other.receiver_id,
      "Menu dispatches one stable receiver identity to the existing service");
  service.Handle(TrayEvent::activate, snapshot, labels);
  Require(commands.back().action == TrayAction::flyout,
          "Mouse or keyboard activation opens the existing current-receiver "
          "flyout");
  service.Handle(TrayEvent::shell_restarted, snapshot, labels);
  Require(shell.added == 2 && commands.size() == 2,
          "Explorer restart rebuilds icon without recreating session or "
          "dispatching connect");
  service.Handle(TrayEvent::open, snapshot, labels);
  Require(commands.back().action == TrayAction::open,
          "Double click opens the main window through the existing command");
  Require(shell.added_states == std::vector<bool>({false, true}),
          "Explorer restart restores the current connected icon");
  auto projection = ProjectTray(snapshot);
  Require(
      projection.receiver_id == current.receiver_id && projection.adjustable &&
          projection.model == "HomePod mini" &&
          projection.detail == "HomePod mini | 127.0.0.1",
      "Flyout projects active hidden receiver and actual command availability");
  session->session->state = SessionState::streaming;
  for (auto standby : {StandbyState::active, StandbyState::silence_pending,
                       StandbyState::standby, StandbyState::resuming}) {
    session->session->standby_state = standby;
    auto tray = ProjectTray(snapshot);
    auto expected =
        standby == StandbyState::standby ? L"StatusStandby" : L"StatusPlaying";
    Require(
        std::wstring(ConnectedStatusKey(tray.state, tray.standby)) == expected,
        "Tray and playback show the same status through standby and resume");
  }
  session->session->state = SessionState::ready;
  session->session->failure_kind = "authentication";
  Require(ProjectTray(snapshot).authentication,
          "Tray keeps authentication prompts distinct from playback status");
  session->session->failure_kind.clear();
  snapshot.volume_pending = true;
  Require(ProjectTray(snapshot).adjustable,
          "Waiting for a remote reply keeps the volume thumb adjustable");
  snapshot.presentation_pending = true;
  Require(!ProjectTray(snapshot).adjustable,
          "Stream reconfiguration disables volume until it completes");
  Require(WindowClose(true, true) == CloseAction::hide &&
              WindowClose(false, true) == CloseAction::exit &&
              WindowClose(true, false) == CloseAction::exit,
          "Closing hides only when a usable tray icon can reopen the window");
  shell.fail_remove = true;
  Rejects([&] { service.Stop(); },
          "Icon removal failure is reported and retains ownership");
  shell.fail_remove = false;
  service.Stop();
  service.Stop();
  Require(shell.removed == 1 && !service.Visible(),
          "Final exit removes owned icon exactly once");
  shell.fail_add = true;
  Rejects([&] { service.Start(); },
          "Missing shell does not claim a visible notification icon");
  auto opened = std::chrono::steady_clock::time_point{};
  Require(TrayFlyoutClick(false, {}, opened) == TrayFlyoutAction::show,
          "The first tray click opens the flyout");
  Require(TrayFlyoutClick(true, {}, opened) == TrayFlyoutAction::hide,
          "A tray click while the flyout is open closes it");
  auto dismissed = opened + std::chrono::milliseconds(20);
  Require(TrayFlyoutClick(false, dismissed,
                          dismissed + std::chrono::milliseconds(30)) ==
              TrayFlyoutAction::keep_closed,
          "The click that dismisses the flyout leaves it closed");
  Require(TrayFlyoutClick(false, dismissed, dismissed + kTrayFlyoutToggle) ==
              TrayFlyoutAction::show,
          "A later tray click opens the flyout again");
}
