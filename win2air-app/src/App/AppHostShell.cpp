#include "AppHost.h"

namespace Win2Air {
void AppHost::LoadPlaybackPages() {
  using namespace winrt::Microsoft::UI::Xaml;
  if (!theme_resources_loaded) {
    auto bytes = StorageIO::Read(xaml_root / L"ThemeResources.xaml", 65536);
    Resources().MergedDictionaries().Append(
        Markup::XamlReader::Load(
            winrt::to_hstring(std::string(bytes.begin(), bytes.end())))
            .as<ResourceDictionary>());
    theme_resources_loaded = true;
  }

  playback = std::make_unique<PlaybackPage>(
      *streaming, xaml_root / L"PlaybackPage.xaml", window.DispatcherQueue());
  devices = std::make_unique<DevicesPage>(
      *streaming, xaml_root / L"DevicesPage.xaml", window.DispatcherQueue());
  playback->SetOpenDevices([this] { PromptAddReceiver(); });
  playback->SetOpenAudio([this] {
    Navigate(AppPage::settings);
    preferences->SelectSection(1);
  });
}
void AppHost::Navigate(AppPage page) {
  navigation.Navigate(page);
  if (!shell) return;
  using namespace winrt::Microsoft::UI::Xaml::Controls;
  auto content = page == AppPage::playback   ? playback->Content()
                 : page == AppPage::devices  ? settings_page
                 : page == AppPage::settings ? settings_page
                                             : statistics_page;
  // Pages are retained by the host. Keep their visual resources loaded too,
  // so navigation does not recreate chart rendering contexts on every visit.
  auto pages = ShellControl<Grid>(L"PageContent").Children();
  for (auto const& child : pages)
    child.Visibility(child == content
                         ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                         : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
  uint32_t index{};
  content.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
  if (!pages.IndexOf(content, index)) pages.Append(content);
  auto control = page == AppPage::playback     ? L"PlaybackNavigation"
                 : page == AppPage::statistics ? L"StatisticsNavigation"
                                               : L"SettingsNavigation";
  rendering_shell = true;
  shell.as<NavigationView>().SelectedItem(
      ShellControl<NavigationViewItem>(control));
  rendering_shell = false;
  if (page == AppPage::devices) preferences->SelectSection(3);
}
winrt::Microsoft::UI::Xaml::FrameworkElement AppHost::LoadXaml(
    wchar_t const* name) {
  auto bytes = StorageIO::Read(xaml_root / name, 65536);
  auto root = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(
                  winrt::to_hstring(std::string(bytes.begin(), bytes.end())))
                  .as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
  LocalizedResources{}.ApplyLanguage(root);
  return root;
}
void AppHost::BuildShell(bool packaged) {
  packaged_application = packaged;
  applied_theme.clear();
  using namespace winrt::Microsoft::UI::Xaml;
  using namespace winrt::Microsoft::UI::Xaml::Controls;
  LocalizedResources resources;
  shell = LoadXaml(L"MainWindow.xaml");
  for (auto name :
       {L"PlaybackNavigation", L"StatisticsNavigation", L"SettingsNavigation"})
    ShellControl<NavigationViewItem>(name).Content(
        winrt::box_value(resources.GetString(name)));
  shell.as<NavigationView>().SelectionChanged(
      [this](auto const&, auto const& args) {
        if (rendering_shell || !args.SelectedItem()) return;
        auto tag = winrt::unbox_value<winrt::hstring>(
            args.SelectedItem().as<NavigationViewItem>().Tag());
        Navigate(ParseActivation(L"--open " + std::wstring(tag.c_str())).page);
      });
  HWND owner{};
  winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&owner));
  statistics = std::make_unique<StatisticsPage>(
      *streaming, xaml_root / L"StatisticsPage.xaml", owner);
  statistics_page = statistics->Content();
  preferences = std::make_unique<SettingsPage>(
      *streaming, xaml_root / L"SettingsPage.xaml", settings_draft);
  settings_page = preferences->Content();
  settings_page.FindName(L"ReceiverManagementContent")
      .as<ContentControl>()
      .Content(devices->Content());
  streaming_settings = std::make_unique<StreamingSettings>(
      *streaming, xaml_root / L"StreamingSettings.xaml", streaming_draft);
  settings_page.FindName(L"StreamingSettingsContent")
      .as<ContentControl>()
      .Content(streaming_settings->Content());
  for (auto name : {L"StartupEnabled", L"StartupMinimized", L"RestorePrevious",
                    L"CloseToTray"}) {
    resources.LocalizeSwitch(settings_page.FindName(name).as<ToggleSwitch>());
  }
  auto setting = [this](wchar_t const* name) {
    return settings_page.FindName(name).as<ToggleSwitch>();
  };
  startup = std::make_unique<StartupService>(NativeStartupBackend(packaged));
  startup_pending.emplace(startup->Query());
  setting(L"StartupEnabled").Toggled([this, setting](auto const&, auto const&) {
    if (rendering_shell || startup_pending) return;
    auto enabled = setting(L"StartupEnabled").IsOn();
    startup_pending.emplace(startup->SetEnabled(enabled, [this](bool value) {
      auto saved = streaming->SetStartup(value).get();
      if (saved.status != CommandStatus::applied)
        throw std::runtime_error(saved.message);
    }));
  });
  shell.ActualThemeChanged([this](auto const&, auto const&) {
    window.DispatcherQueue().TryEnqueue([this] { UpdateTitleBar(); });
  });
  window.Content(shell);
  UpdateTitleBar();
}
void AppHost::UpdateTitleBar() {
  using namespace winrt::Microsoft::UI::Xaml;
  using namespace winrt::Microsoft::UI::Xaml::Media;
  auto navigation_control = shell.as<Controls::NavigationView>();
  auto background =
      navigation_control.Background().as<SolidColorBrush>().Color();
  auto foreground =
      navigation_control.Foreground().as<SolidColorBrush>().Color();
  HWND handle{};
  winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&handle));
  BOOL dark = shell.ActualTheme() == ElementTheme::Dark;
  winrt::check_hresult(DwmSetWindowAttribute(
      handle, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)));
  auto set_native_color = [&](DWORD attribute, COLORREF color) {
    auto result =
        DwmSetWindowAttribute(handle, attribute, &color, sizeof(color));
    if (result != E_INVALIDARG) winrt::check_hresult(result);
  };
  set_native_color(DWMWA_CAPTION_COLOR,
                   RGB(background.R, background.G, background.B));
  set_native_color(DWMWA_TEXT_COLOR,
                   RGB(foreground.R, foreground.G, foreground.B));
  auto title = window.AppWindow().TitleBar();
  title.BackgroundColor(background);
  title.InactiveBackgroundColor(background);
  title.ForegroundColor(foreground);
  title.InactiveForegroundColor(foreground);
  title.ButtonBackgroundColor(background);
  title.ButtonInactiveBackgroundColor(background);
  title.ButtonForegroundColor(foreground);
  title.ButtonInactiveForegroundColor(foreground);
  auto overlay = [&](unsigned percent) {
    auto channel = [&](unsigned back, unsigned front) {
      return static_cast<uint8_t>((back * (100 - percent) + front * percent) /
                                  100);
    };
    return winrt::Windows::UI::Color{255, channel(background.R, foreground.R),
                                     channel(background.G, foreground.G),
                                     channel(background.B, foreground.B)};
  };
  title.ButtonHoverBackgroundColor(overlay(10));
  title.ButtonPressedBackgroundColor(overlay(20));
  title.ButtonHoverForegroundColor(foreground);
  title.ButtonPressedForegroundColor(foreground);
}
void AppHost::RenderShell() {
  if (!shell) return;
  using namespace winrt::Microsoft::UI::Xaml::Controls;
  streaming_settings->Refresh();
  statistics->Refresh(navigation.Page() == AppPage::statistics &&
                      window.AppWindow().IsVisible());
  preferences->Refresh(statistics->ExportBusy() || startup_pending.has_value());
  LocalizedResources resources;
  auto snapshot = streaming->Snapshot();
  if (tray) tray->Update(snapshot);
  std::vector<std::string> failures{
      snapshot.discovery_error,  snapshot.local_error,
      snapshot.credential_error, snapshot.metrics_error,
      snapshot.adaptive_error,   snapshot.diagnostics_error,
      snapshot.metadata_error,   snapshot.source.error};
  if (snapshot.session && snapshot.session->session &&
      snapshot.session->session->error)
    failures.push_back(snapshot.session->session->error->message);
  for (auto const& failure : failures)
    if (std::find(logged_failures_.begin(), logged_failures_.end(), failure) ==
        logged_failures_.end())
      LogUiFailure(failure);
  logged_failures_ = std::move(failures);
  if (snapshot.preferences.language != applied_language && !startup_pending &&
      !preferences->Pending() && !streaming_settings->Pending() &&
      !statistics->ExportBusy() && !shutdown) {
    auto page = navigation.Page();
    ConfigureResources(snapshot.preferences.language);
    applied_language = snapshot.preferences.language;
    LoadPlaybackPages();
    BuildShell(packaged_application);
    Navigate(page);
  }
  if (snapshot.preferences.theme != applied_theme) {
    auto theme = ResolveTheme(snapshot.preferences.theme);
    auto requested = theme == ThemePreference::system
                         ? winrt::Microsoft::UI::Xaml::ElementTheme::Default
                     : theme == ThemePreference::light
                         ? winrt::Microsoft::UI::Xaml::ElementTheme::Light
                         : winrt::Microsoft::UI::Xaml::ElementTheme::Dark;
    for (auto const& root :
         {shell, playback->Content(), devices->Content(), settings_page,
          statistics_page, streaming_settings->Content()})
      root.RequestedTheme(requested);
    applied_theme = snapshot.preferences.theme;
  }
  if (flyout_content) {
    flyout_content.RequestedTheme(shell.RequestedTheme());
    if (flyout_content.Language() != resources.TextLanguage())
      resources.ApplyLanguage(flyout_content);
  }
  preferences->Refresh(statistics->ExportBusy() || startup_pending.has_value());
  rendering_shell = true;
  auto enabled = startup_state.state == StartupState::enabled ||
                 startup_state.state == StartupState::enabled_by_policy;
  auto startup_control =
      settings_page.FindName(L"StartupEnabled").as<ToggleSwitch>();
  startup_control.IsOn(enabled);
  startup_control.IsEnabled(!startup_pending &&
                            (startup_state.state == StartupState::disabled ||
                             startup_state.state == StartupState::enabled));
  winrt::hstring startup_text;
  switch (startup_state.state) {
    case StartupState::disabled_by_user:
      startup_text = resources.GetString(L"StartupDisabledByUser");
      break;
    case StartupState::disabled_by_policy:
      startup_text = resources.GetString(L"StartupDisabledByPolicy");
      break;
    case StartupState::enabled_by_policy:
      startup_text = resources.GetString(L"StartupEnabledByPolicy");
      break;
    default:
      break;
  }
  auto startup_status =
      settings_page.FindName(L"StartupStatus").as<TextBlock>();
  startup_status.Text(startup_text);
  startup_status.Visibility(
      startup_text.empty() ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                           : winrt::Microsoft::UI::Xaml::Visibility::Visible);
  tray_model->Refresh();
  if (flyout_content) {
    flyout_content.FindName(L"TrayReceiverVolume")
        .as<Slider>()
        .SetValue(winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::
                      NameProperty(),
                  winrt::box_value(resources.GetString(L"ReceiverVolume")));

    flyout_content.FindName(L"TrayOpen")
        .as<Button>()
        .Content(winrt::box_value(resources.GetString(L"TrayOpen")));
    auto const& view = tray_model->Snapshot();
    flyout_content.FindName(L"TrayReceiver")
        .as<TextBlock>()
        .Text(view.name.empty() ? resources.GetString(L"NoActiveReceiver")
                                : winrt::to_hstring(view.name));
    auto model = flyout_content.FindName(L"TrayReceiverModel").as<TextBlock>();
    model.Text(winrt::to_hstring(view.detail));
    model.Visibility(view.detail.empty()
                         ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                         : winrt::Microsoft::UI::Xaml::Visibility::Visible);
    flyout_content.FindName(L"TrayReceiverVolume")
        .as<Slider>()
        .Value(view.receiver_volume * 100);
    flyout_content.FindName(L"TrayReceiverMuted")
        .as<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton>()
        .IsChecked(view.muted);
    flyout_content.FindName(L"TrayMuteIcon")
        .as<FontIcon>()
        .Glyph(view.muted ? L"\uE198" : L"\uE15D");
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        flyout_content.FindName(L"TrayReceiverMuted")
            .as<winrt::Microsoft::UI::Xaml::DependencyObject>(),
        resources.GetString(view.muted ? L"UnmuteAction" : L"MuteAction"));
    flyout_content.FindName(L"TrayVolumeValue")
        .as<TextBlock>()
        .Text(std::to_wstring(
                  view.muted ? 0 : std::lround(view.receiver_volume * 100)) +
              L"%");
    auto connected = view.state != SessionState::disconnected &&
                     view.state != SessionState::error;
    flyout_content.FindName(L"TrayConnection")
        .as<Button>()
        .Content(winrt::box_value(
            resources.GetString(connected ? L"Disconnect" : L"Connect")));
    for (auto name : {L"TrayReceiverVolume", L"TrayReceiverMuted"})
      flyout_content.FindName(name)
          .as<winrt::Microsoft::UI::Xaml::Controls::Control>()
          .IsEnabled(view.adjustable);
    auto retry = view.retry_seconds
                     ? resources.GetString(L"RetryDue") + L" " +
                           winrt::to_hstring(*view.retry_seconds) + L" s"
                     : winrt::hstring{};
    flyout_content.FindName(L"TrayError")
        .as<winrt::Microsoft::UI::Xaml::FrameworkElement>()
        .Visibility(retry.empty()
                        ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                        : winrt::Microsoft::UI::Xaml::Visibility::Visible);
    flyout_content.FindName(L"TrayError").as<TextBlock>().Text(retry);
    auto connected_key = ConnectedStatusKey(view.state, view.standby);
    auto state_key =
        view.authentication                      ? L"AuthenticationRequired"
        : connected_key                          ? connected_key
        : view.state == SessionState::streaming  ? L"StateStreaming"
        : view.state == SessionState::ready      ? L"StateReady"
        : view.state == SessionState::pairing    ? L"StatePairing"
        : view.state == SessionState::setting_up ? L"StateSettingUp"
        : view.state == SessionState::connecting ? L"StateConnecting"
        : view.state == SessionState::recovering ? L"StateRecovering"
        : view.state == SessionState::error      ? L"StateError"
        : view.state == SessionState::ending     ? L"StateEnding"
                                                 : L"StateDisconnected";
    auto indicator = flyout_content.FindName(L"TrayStateIndicator")
                         .as<winrt::Microsoft::UI::Xaml::Shapes::Ellipse>();
    indicator.Visibility(
        connected_key && !view.authentication
            ? winrt::Microsoft::UI::Xaml::Visibility::Visible
            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    flyout_content.FindName(L"TrayState")
        .as<TextBlock>()
        .Foreground(connected_key && !view.authentication ? indicator.Fill()
                                                          : model.Foreground());
    flyout_content.FindName(L"TrayState")
        .as<TextBlock>()
        .Text(resources.GetString(state_key));
  }
  rendering_shell = false;
  if (flyout && flyout.AppWindow().IsVisible()) LayoutFlyout();
}
void AppHost::TickShell() {
  if (!shell) return;
  if (instance) {
    auto error = instance->TakeError();
    if (!error.empty()) ShowError(error);
    for (auto const& request : instance->Queue()->Take())
      ApplyActivation(request);
  }
  if (startup_pending && startup_pending->wait_for(std::chrono::seconds(0)) ==
                             std::future_status::ready) {
    startup_state = startup_pending->get();
    startup_pending.reset();
    if (!startup_state.error.empty()) ShowError(startup_state.error);
  }
  for (auto item = shell_pending.begin(); item != shell_pending.end();) {
    if (item->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++item;
      continue;
    }
    auto result = item->get();
    if (result.status == CommandStatus::failed ||
        result.status == CommandStatus::rejected)
      ShowError(result.message);
    item = shell_pending.erase(item);
  }
  RenderShell();
}
}  // namespace Win2Air
