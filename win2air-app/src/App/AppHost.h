#pragma once
#include <windows.h>
#undef GetCurrentTime
#undef GetClassName
#include <appmodel.h>
#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Microsoft.Windows.Storage.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../Diagnostics/Logger.h"
#include "../Settings/SettingsStore.h"
#include "../Settings/StorageIO.h"
#include "../Shell/StartupService.h"
#include "../Shell/TrayService.h"
#include "../Streaming/CommandDispatcher.h"
#include "../Streaming/StreamCoordinator.h"
#include "../UI/DevicesPage.h"
#include "../UI/PlaybackPage.h"
#include "../UI/ReceiverDialogs.h"
#include "../UI/SettingsPage.h"
#include "../UI/StatisticsPage.h"
#include "../UI/StreamingSettings.h"
#include "../UI/ViewModels/NavigationViewModel.h"
#include "../UI/ViewModels/TrayViewModel.h"
#include "../UI/ViewModels/ViewModelBase.h"
#include "PowerNotifications.h"
#include "SingleInstance.h"
#include "StateRestore.h"
#include "StorePreview.h"
#include "SystemAudioValidation.h"
namespace Win2Air {
inline std::atomic<int> validation_exit{};
struct AppHost
    : winrt::Microsoft::UI::Xaml::ApplicationT<
          AppHost, winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider> {
  winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider
      metadata;
  winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider chart_metadata{
      nullptr};
  std::filesystem::path startup_report;
  std::string startup_operation{"Read activation arguments"};
  std::wstring command_line;
  NativeInstance* instance{};
  std::optional<ActivationRequest> initial_activation;
  explicit AppHost(std::wstring arguments, NativeInstance* native = nullptr)
      : command_line(std::move(arguments)), instance(native) {
    if (instance) initial_activation = instance->Initial();
    UnhandledException([this](auto const&, auto const& failure) {
      validation_exit = E_FAIL;
      if (startup_report.empty()) return;
      try {
        winrt::Windows::Data::Json::JsonObject report;
        report.Insert(L"status",
                      winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                          L"failed"));
        report.Insert(L"error",
                      winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                          failure.Message()));
        StorageIO::AtomicText(startup_report,
                              winrt::to_string(report.Stringify()));
      } catch (...) {
        OutputDebugStringW(L"Writing the GUI validation failure failed");
      }
    });
  }
  winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(
      winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
    if (auto result = metadata.GetXamlType(type)) return result;
    if (!chart_metadata)
      chart_metadata =
          winrt::get_activation_factory<
              winrt::Windows::Foundation::IActivationFactory>(
              L"Win2Air.Charts.Win2Air_Charts_XamlTypeInfo."
              L"XamlMetaDataProvider")
              .ActivateInstance<
                  winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider>();
    return chart_metadata.GetXamlType(type);
  }
  winrt::Microsoft::UI::Xaml::Markup::IXamlType GetXamlType(
      winrt::hstring const& name) {
    if (auto result = metadata.GetXamlType(name)) return result;
    if (!chart_metadata)
      chart_metadata =
          winrt::get_activation_factory<
              winrt::Windows::Foundation::IActivationFactory>(
              L"Win2Air.Charts.Win2Air_Charts_XamlTypeInfo."
              L"XamlMetaDataProvider")
              .ActivateInstance<
                  winrt::Microsoft::UI::Xaml::Markup::IXamlMetadataProvider>();
    return chart_metadata.GetXamlType(name);
  }
  winrt::com_array<winrt::Microsoft::UI::Xaml::Markup::XmlnsDefinition>
  GetXmlnsDefinitions() {
    return metadata.GetXmlnsDefinitions();
  }
  winrt::Microsoft::UI::Xaml::Window window{nullptr};
  winrt::Microsoft::UI::Xaml::DispatcherTimer timer{nullptr};
  std::shared_ptr<Logger> logger;
  std::unique_ptr<SettingsStore> settings;
  std::unique_ptr<CredentialStore> credentials;
  std::unique_ptr<SenderLibrary> library;
  std::unique_ptr<CommandDispatcher> commands;
  std::unique_ptr<StreamCoordinator> streaming;
  std::unique_ptr<PowerNotifications> power;
  std::unique_ptr<PlaybackPage> playback;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog receiver_dialog{nullptr};
  std::unique_ptr<DevicesPage> devices;
  std::unique_ptr<StreamingSettings> streaming_settings;
  std::unique_ptr<StatisticsPage> statistics;
  std::unique_ptr<SettingsPage> preferences;
  std::shared_ptr<SettingsDraft> settings_draft =
      std::make_shared<SettingsDraft>();
  std::shared_ptr<StreamingDraft> streaming_draft =
      std::make_shared<StreamingDraft>();
  std::string applied_theme, applied_language;
  bool theme_resources_loaded{};
  bool render_review{}, review_started{}, store_screenshots{};
  std::filesystem::path store_dir;
  bool packaged_application{};
  winrt::Microsoft::UI::Xaml::DispatcherTimer refresh{nullptr};
  std::unique_ptr<SystemAudioValidation> audio_validation;
  std::optional<std::shared_future<CommandResult>> shutdown;
  bool allow_close{};
  winrt::Microsoft::UI::Xaml::FrameworkElement shell{nullptr},
      settings_page{nullptr}, statistics_page{nullptr}, flyout_content{nullptr};
  winrt::Microsoft::UI::Xaml::Window flyout{nullptr};
  winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendered_revoker
      flyout_rendered;
  std::optional<std::chrono::steady_clock::time_point> flyout_hidden_at;
  std::optional<TrayPointer> tray_click;
  NavigationViewModel navigation;
  std::unique_ptr<TrayService> tray;
  std::unique_ptr<TrayViewModel> tray_model;
  std::unique_ptr<StartupService> startup;
  std::optional<std::future<StartupResult>> startup_pending;
  StartupResult startup_state{StartupState::unavailable, false, ""};
  std::vector<std::future<CommandResult>> shell_pending;
  std::filesystem::path xaml_root;
  bool rendering_shell{};
  FailurePrompt shell_prompt_;
  std::vector<std::string> logged_failures_;
  std::filesystem::path ui_report;
  std::chrono::steady_clock::time_point ui_due;
  winrt::com_ptr<ViewModelBase> view_model;
  unsigned snapshot_updates{};

  void RequestClose();
  void ShowError(std::string const& message);
  WindowPlacement Placement();
  void SavePlacement() {
    if (streaming && window)
      shell_pending.push_back(streaming->SaveWindow(Placement()));
  }
  template <class T>
  T ShellControl(wchar_t const* name) {
    return shell.FindName(name).as<T>();
  }
  void LoadPlaybackPages();
  void PromptAddReceiver();
  void PromptPairing(std::string identity);
  void Navigate(AppPage page);
  void ApplyActivation(ActivationRequest const& request, bool show = true);
  winrt::Microsoft::UI::Xaml::FrameworkElement LoadXaml(wchar_t const* name);
  void BuildShell(bool packaged);
  void UpdateTitleBar();
  void HideFlyout();
  void OpenMainWindow();
  void PrepareFlyout();
  void ShowFlyout();
  void LayoutFlyout();
  winrt::fire_and_forget CaptureReview();
  RECT VisibleWindowRect(HWND handle);
  void SizeStoreWindow();
  void ApplyStoreLanguage(std::string const& language);
  winrt::fire_and_forget CaptureStore();
  void RenderShell();
  void TickShell();
  void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
  void StartupFailed(std::string const& message);
  void Launch();
};
}  // namespace Win2Air
