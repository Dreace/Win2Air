#include "AppHost.h"

namespace Win2Air {
RECT AppHost::VisibleWindowRect(HWND handle) {
  RECT visible{};
  if (DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &visible,
                            sizeof(visible)) == S_OK)
    return visible;
  if (!GetWindowRect(handle, &visible)) winrt::throw_last_error();
  return visible;
}
void AppHost::SizeStoreWindow() {
  using namespace winrt::Microsoft::UI::Windowing;
  HWND handle{};
  winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&handle));
  auto presenter = window.AppWindow().Presenter().try_as<OverlappedPresenter>();
  if (!presenter)
    throw std::runtime_error("Store screenshots require an overlapped window");
  if (presenter.State() == OverlappedPresenterState::Maximized ||
      presenter.State() == OverlappedPresenterState::Minimized)
    presenter.Restore();
  RECT outer{};
  if (!GetWindowRect(handle, &outer)) winrt::throw_last_error();
  auto visible = VisibleWindowRect(handle);
  auto frame_width =
      (outer.right - outer.left) - (visible.right - visible.left);
  auto frame_height =
      (outer.bottom - outer.top) - (visible.bottom - visible.top);
  if (frame_width < 0) frame_width = 0;
  if (frame_height < 0) frame_height = 0;
  constexpr int width = 1920;
  constexpr int height = 1080;
  auto outer_width = width + frame_width;
  auto outer_height = height + frame_height;
  MONITORINFO monitor{sizeof(monitor)};
  if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST),
                       &monitor))
    winrt::throw_last_error();
  auto work_width = monitor.rcWork.right - monitor.rcWork.left;
  auto work_height = monitor.rcWork.bottom - monitor.rcWork.top;
  int x = monitor.rcWork.left - (visible.left - outer.left);
  int y = monitor.rcWork.top - (visible.top - outer.top);
  if (work_width > width) x += (work_width - width) / 2;
  if (work_height > height) y += (work_height - height) / 2;
  window.AppWindow().MoveAndResize({x, y, outer_width, outer_height});
  window.Activate();
}
void AppHost::ApplyStoreLanguage(std::string const& language) {
  ConfigureResources(language);
  applied_language = language;
  applied_theme.clear();
  LoadPlaybackPages();
  BuildShell(packaged_application);
  using namespace winrt::Microsoft::UI::Xaml;
  shell.as<Controls::NavigationView>().IsPaneOpen(true);
  for (auto const& root :
       {shell, playback->Content(), devices->Content(), settings_page,
        statistics_page, streaming_settings->Content()})
    root.RequestedTheme(ElementTheme::Light);
  applied_theme = "light";
  UpdateTitleBar();
  SizeStoreWindow();
}
// CaptureStore must not co_await an IAsyncAction coroutine while holding an
// apartment_context: MSVC 19.51 crashes compiling that combination in the
// Release configuration, so the acknowledgement wait is inlined here.
winrt::fire_and_forget AppHost::CaptureStore() {
  auto lifetime = get_strong();
  refresh.Stop();
  winrt::apartment_context ui;
  try {
    struct Step {
      char const* id;
      char const* language;
      AppPage page;
    };
    Step steps[] = {{"zh-CN-playback", "zh-CN", AppPage::playback},
                    {"zh-CN-statistics", "zh-CN", AppPage::statistics},
                    {"zh-CN-settings", "zh-CN", AppPage::settings},
                    {"en-US-playback", "en-US", AppPage::playback},
                    {"en-US-statistics", "en-US", AppPage::statistics},
                    {"en-US-settings", "en-US", AppPage::settings}};
    std::string shown;
    std::optional<PlaybackSnapshot> preview;
    for (auto const& step : steps) {
      if (shown != step.language) {
        ApplyStoreLanguage(step.language);
        preview = StorePreview(step.language);
      }
      shown = step.language;
      Navigate(step.page);
      if (step.page == AppPage::playback) playback->Present(*preview);
      if (step.page == AppPage::statistics) statistics->Present(*preview);
      if (step.page == AppPage::settings) {
        preferences->SelectSection(0);
        preferences->Present(*preview);
      }
      if (step.page == AppPage::settings) {
        rendering_shell = true;
        auto startup_control =
            settings_page.FindName(L"StartupEnabled")
                .as<winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch>();
        startup_control.IsOn(false);
        startup_control.IsEnabled(true);
        auto status =
            settings_page.FindName(L"StartupStatus")
                .as<winrt::Microsoft::UI::Xaml::Controls::TextBlock>();
        status.Text(L"");
        status.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
        rendering_shell = false;
      }
      shell.UpdateLayout();
      HWND handle{};
      winrt::check_hresult(
          window.as<IWindowNative>()->get_WindowHandle(&handle));
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      for (;;) {
        auto visible = VisibleWindowRect(handle);
        auto width = visible.right - visible.left;
        auto height = visible.bottom - visible.top;
        if (width == 1920 && height == 1080 && shell.ActualWidth() >= 400 &&
            shell.ActualHeight() >= 300)
          break;
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error(
              "Store screenshot window is " + std::to_string(width) + "x" +
              std::to_string(height) + "; expected 1920x1080");
        SizeStoreWindow();
        co_await winrt::resume_after(std::chrono::milliseconds(50));
        co_await ui;
        shell.UpdateLayout();
      }
      co_await winrt::resume_after(std::chrono::milliseconds(900));
      co_await ui;
      std::filesystem::remove(store_dir / L"ack.txt");
      StorageIO::AtomicText(store_dir / L"ready.txt", step.id);
      auto ack = store_dir / L"ack.txt";
      auto ack_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(30);
      bool acknowledged = false;
      while (std::chrono::steady_clock::now() < ack_deadline) {
        co_await winrt::resume_after(std::chrono::milliseconds(50));
        co_await ui;
        if (!std::filesystem::exists(ack)) continue;
        auto bytes = StorageIO::Read(ack, 256);
        if (std::string(bytes.begin(), bytes.end()) == step.id) {
          acknowledged = true;
          break;
        }
      }
      if (!acknowledged)
        throw std::runtime_error(
            "Store screenshot capture did not acknowledge " +
            std::string(step.id));
      co_await ui;
    }
  } catch (...) {
    validation_exit = E_FAIL;
    try {
      StorageIO::AtomicText(store_dir / L"error.txt",
                            DescribeFailure().message);
    } catch (...) {
    }
  }
  refresh.Start();
  RequestClose();
}
}  // namespace Win2Air
