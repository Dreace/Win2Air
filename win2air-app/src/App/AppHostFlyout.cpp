#include "AppHost.h"

namespace Win2Air {
void AppHost::HideFlyout() {
  flyout_rendered.revoke();
  if (flyout) flyout.AppWindow().Hide();
}
void AppHost::OpenMainWindow() {
  HideFlyout();
  HWND handle{};
  winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&handle));
  ShowWindow(handle, IsIconic(handle) ? SW_RESTORE : SW_SHOW);
  window.AppWindow().Show();
  window.Activate();
  SetForegroundWindow(handle);
}
void AppHost::PrepareFlyout() {
  using namespace winrt::Microsoft::UI::Xaml;
  using namespace winrt::Microsoft::UI::Xaml::Controls;
  if (!flyout) {
    flyout = Window();
    flyout.ExtendsContentIntoTitleBar(true);
    flyout.AppWindow().IsShownInSwitchers(false);
    auto presenter =
        flyout.AppWindow()
            .Presenter()
            .as<winrt::Microsoft::UI::Windowing::OverlappedPresenter>();
    presenter.SetBorderAndTitleBar(false, false);
    presenter.IsResizable(false);
    presenter.IsMaximizable(false);
    presenter.IsMinimizable(false);
    presenter.IsAlwaysOnTop(true);
    HWND handle{};
    winrt::check_hresult(flyout.as<IWindowNative>()->get_WindowHandle(&handle));
    auto style = GetWindowLongPtrW(handle, GWL_STYLE);
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(
            handle, GWL_STYLE,
            (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME)) |
                WS_POPUP) &&
        GetLastError() != ERROR_SUCCESS)
      winrt::throw_last_error();
    auto extended = GetWindowLongPtrW(handle, GWL_EXSTYLE);
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(
            handle, GWL_EXSTYLE,
            extended & ~(WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE |
                         WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE)) &&
        GetLastError() != ERROR_SUCCESS)
      winrt::throw_last_error();
    COLORREF border = DWMWA_COLOR_NONE;
    auto border_result = DwmSetWindowAttribute(handle, DWMWA_BORDER_COLOR,
                                               &border, sizeof(border));
    if (border_result != E_INVALIDARG) winrt::check_hresult(border_result);
    if (!SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                      SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                          SWP_NOZORDER | SWP_NOACTIVATE))
      winrt::throw_last_error();
    flyout_content = LoadXaml(L"TrayFlyout.xaml");
    flyout.Content(flyout_content);
    LocalizedResources resources;
    flyout_content.FindName(L"TrayReceiverVolume")
        .as<Slider>()
        .SetValue(winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::
                      NameProperty(),
                  winrt::box_value(resources.GetString(L"ReceiverVolume")));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        flyout_content.FindName(L"TrayReceiverMuted")
            .as<winrt::Microsoft::UI::Xaml::DependencyObject>(),
        resources.GetString(L"MuteAction"));
    flyout_content.FindName(L"TrayOpen")
        .as<Button>()
        .Content(winrt::box_value(resources.GetString(L"TrayOpen")));
    flyout_content.FindName(L"TrayOpen")
        .as<Button>()
        .Click([this](auto const&, auto const&) { OpenMainWindow(); });
    flyout_content.FindName(L"TrayReceiverVolume")
        .as<Slider>()
        .ValueChanged([this](auto const&, auto const&) {
          if (!rendering_shell)
            tray_model->Volume(flyout_content.FindName(L"TrayReceiverVolume")
                                   .as<Slider>()
                                   .Value() /
                               100);
        });
    flyout_content.FindName(L"TrayReceiverMuted")
        .as<Primitives::ToggleButton>()
        .Click([this](auto const&, auto const&) {
          if (!rendering_shell)
            tray_model->Mute(flyout_content.FindName(L"TrayReceiverMuted")
                                 .as<Primitives::ToggleButton>()
                                 .IsChecked()
                                 .Value());
        });
    flyout.AppWindow().Closing([this](auto const&, auto const& args) {
      if (!allow_close) {
        args.Cancel(true);
        HideFlyout();
      }
    });
    flyout.Activated([this](auto const&, auto const& args) {
      if (args.WindowActivationState() == WindowActivationState::Deactivated) {
        flyout_hidden_at = std::chrono::steady_clock::now();
        HideFlyout();
      }
    });
    flyout_content.FindName(L"TrayConnection")
        .as<Button>()
        .Click([this](auto const&, auto const&) {
          auto view = tray_model->Snapshot();
          bool connected = view.state != SessionState::disconnected &&
                           view.state != SessionState::error;
          if (connected)
            shell_pending.push_back(streaming->Disconnect());
          else if (!view.receiver_id.empty())
            shell_pending.push_back(streaming->Connect(view.receiver_id));
          else {
            HideFlyout();
            PromptAddReceiver();
          }
        });
    flyout_content.FindName(L"TraySwitch")
        .as<Button>()
        .Content(winrt::box_value(resources.GetString(L"TraySwitch")));
    MenuFlyout targets;
    targets.Opening([this](auto const& sender, auto const&) {
      auto menu = sender.template as<MenuFlyout>();
      menu.Items().Clear();
      auto state = streaming->Snapshot();
      LocalizedResources labels;
      for (auto const& entry :
           TrayMenu(state, {labels.GetString(L"TrayOpen").c_str(),
                            labels.GetString(L"SettingsNavigation").c_str(),
                            labels.GetString(L"TrayExit").c_str(),
                            labels.GetString(L"Disconnect").c_str()})) {
        if (entry.command.action != TrayAction::connect) continue;
        ToggleMenuFlyoutItem item;
        item.Text(entry.label);
        item.IsChecked(entry.checked);
        bool active =
            state.session && state.session->session &&
            state.session->intent.desired_connected &&
            state.session->session->receiver_id == entry.command.receiver_id;
        item.IsEnabled(!active);
        item.Click([this, identity = entry.command.receiver_id](auto const&,
                                                                auto const&) {
          shell_pending.push_back(streaming->Connect(identity));
        });
        menu.Items().Append(item);
      }
    });
    flyout_content.FindName(L"TraySwitch").as<Button>().Flyout(targets);
    auto exit = flyout_content.FindName(L"TrayExit").as<Button>();
    exit.Content(winrt::box_value(resources.GetString(L"TrayExit")));
    exit.Click([this](auto const&, auto const&) { RequestClose(); });
  }
}
void AppHost::ShowFlyout() {
  using namespace winrt::Microsoft::UI::Xaml;
  auto now = std::chrono::steady_clock::now();
  switch (TrayFlyoutClick(flyout && flyout.AppWindow().IsVisible(),
                          flyout_hidden_at, now)) {
    case TrayFlyoutAction::hide:
      flyout_hidden_at = now;
      HideFlyout();
      return;
    case TrayFlyoutAction::keep_closed:
      return;
    case TrayFlyoutAction::show:
      flyout_hidden_at.reset();
      break;
  }
  PrepareFlyout();
  RenderShell();
  LayoutFlyout();
  HWND handle{};
  winrt::check_hresult(flyout.as<IWindowNative>()->get_WindowHandle(&handle));
  BOOL cloak = TRUE;
  winrt::check_hresult(
      DwmSetWindowAttribute(handle, DWMWA_CLOAK, &cloak, sizeof(cloak)));
  flyout_rendered.revoke();
  flyout_rendered = Media::CompositionTarget::Rendered(
      winrt::auto_revoke, [this, handle](auto const&, auto const&) {
        if (logger)
          logger->Write({LogLevel::debug,
                         LogModule::app,
                         LogEvent::application_lifecycle,
                         {},
                         0,
                         0,
                         0,
                         0,
                         0,
                         "operation=tray-frame-rendered loaded=" +
                             std::to_string(flyout_content.IsLoaded())});
        if (!flyout_content.IsLoaded()) return;
        flyout_rendered.revoke();
        try {
          BOOL cloak = FALSE;
          winrt::check_hresult(DwmSetWindowAttribute(handle, DWMWA_CLOAK,
                                                     &cloak, sizeof(cloak)));
          if (flyout.AppWindow().IsVisible()) flyout.Activate();
        } catch (...) {
          HideFlyout();
          ShowError(DescribeFailure().message);
        }
      });
  if (logger)
    logger->Write({LogLevel::debug,
                   LogModule::app,
                   LogEvent::application_lifecycle,
                   {},
                   0,
                   0,
                   0,
                   0,
                   0,
                   "operation=tray-show-prepared"});
  flyout.AppWindow().Show(false);
}
void AppHost::LayoutFlyout() {
  HWND flyout_handle{};
  winrt::check_hresult(
      flyout.as<IWindowNative>()->get_WindowHandle(&flyout_handle));
  auto flyout_dpi = GetDpiForWindow(flyout_handle);
  POINT cursor{};
  if (!tray_click && !GetCursorPos(&cursor))
    throw std::runtime_error("Read tray pointer failed: " +
                             std::to_string(GetLastError()));
  auto pointer_x = tray_click ? tray_click->x : int(cursor.x);
  auto pointer_y = tray_click ? tray_click->y : int(cursor.y);
  MONITORINFO monitor{sizeof(monitor)};
  if (!GetMonitorInfoW(
          MonitorFromPoint({pointer_x, pointer_y}, MONITOR_DEFAULTTONEAREST),
          &monitor))
    winrt::throw_last_error();
  RECT outer{}, client{};
  if (!GetWindowRect(flyout_handle, &outer) ||
      !GetClientRect(flyout_handle, &client))
    winrt::throw_last_error();
  auto frame_width = int(outer.right - outer.left - client.right + client.left);
  auto frame_height =
      int(outer.bottom - outer.top - client.bottom + client.top);
  constexpr int panel_width = 360;
  TrayArea work{monitor.rcWork.left, monitor.rcWork.top,
                monitor.rcWork.right - monitor.rcWork.left,
                monitor.rcWork.bottom - monitor.rcWork.top};
  auto available_height = std::max(1, work.height - frame_height);
  // Measure the full content rather than reusing the current window viewport.
  flyout_content.Measure(
      {float(panel_width), std::numeric_limits<float>::infinity()});
  auto width = MulDiv(panel_width, flyout_dpi, 96) + frame_width;
  auto height = std::min(available_height,
                         int(std::ceil(flyout_content.DesiredSize().Height *
                                       flyout_dpi / 96))) +
                frame_height;
  auto placed = PlaceTrayFlyout(work, pointer_x, pointer_y, width, height);
  if (outer.left != placed.x || outer.top != placed.y ||
      outer.right - outer.left != placed.width ||
      outer.bottom - outer.top != placed.height)
    flyout.AppWindow().MoveAndResize(
        {placed.x, placed.y, placed.width, placed.height});
}
}  // namespace Win2Air
