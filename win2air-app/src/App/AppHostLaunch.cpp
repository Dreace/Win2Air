#include "AppHost.h"

namespace Win2Air {
void AppHost::Launch() {
  UINT32 identity_length{};
  auto identity_status = GetCurrentPackageFullName(&identity_length, nullptr);
  if (identity_status != ERROR_INSUFFICIENT_BUFFER &&
      identity_status != APPMODEL_ERROR_NO_PACKAGE)
    winrt::throw_hresult(HRESULT_FROM_WIN32(identity_status));
  winrt::hstring arguments(command_line);
  if (identity_status != APPMODEL_ERROR_NO_PACKAGE && !initial_activation) {
    const auto activation =
        winrt::Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent()
            .GetActivatedEventArgs();
    if (activation.Kind() !=
        winrt::Microsoft::Windows::AppLifecycle::ExtendedActivationKind::Launch)
      winrt::throw_hresult(E_NOTIMPL);
    arguments = activation.Data()
                    .as<winrt::Windows::ApplicationModel::Activation::
                            ILaunchActivatedEventArgs>()
                    .Arguments();
  }
  std::wstring validation_prefix = L"--validate-system-audio ";
  std::wstring argument_text(arguments.c_str());
  bool validating = argument_text.starts_with(validation_prefix);
  bool reviewing = argument_text.starts_with(L"--review-ui ");
  render_review = argument_text.starts_with(L"--render-ui ");
  std::wstring ui_prefix = render_review ? L"--render-ui "
                           : reviewing   ? L"--review-ui "
                                         : L"--validate-playback-ui ";
  bool inspecting = argument_text.starts_with(ui_prefix);
  store_screenshots = argument_text.starts_with(L"--store-screenshots ");
  FailureDialogsAllowed() = !validating && !inspecting && !store_screenshots &&
                            argument_text != L"--smoke-window";
  if (store_screenshots) {
    std::wstring directory =
        argument_text.substr(std::wstring(L"--store-screenshots ").size());
    if (directory.size() >= 2 && directory.front() == L'"' &&
        directory.back() == L'"')
      directory = directory.substr(1, directory.size() - 2);
    store_dir = directory;
    if (!store_dir.is_absolute())
      throw std::runtime_error(
          "Store screenshots require an absolute output directory");
    std::filesystem::create_directories(store_dir);
    initial_activation = ActivationRequest{};
  }
  if (inspecting) {
    initial_activation = ActivationRequest{};
    ui_report = argument_text.substr(ui_prefix.size());
    if (!ui_report.is_absolute())
      throw std::runtime_error(
          "UI validation requires an absolute report path");
    startup_report = ui_report;
    ui_due = std::chrono::steady_clock::now() +
             std::chrono::seconds(reviewing ? 180 : 6);
  }
  if (!initial_activation && !arguments.empty() &&
      arguments != L"--smoke-window" && !validating && !inspecting)
    winrt::throw_hresult(E_INVALIDARG);
  startup_operation = "Open application data";
  auto data =
      identity_status == APPMODEL_ERROR_NO_PACKAGE
          ? winrt::Microsoft::Windows::Storage::ApplicationData::
                GetForUnpackaged(L"Win2Air", L"Win2Air")
          : winrt::Microsoft::Windows::Storage::ApplicationData::GetDefault();
  auto local = std::filesystem::path(data.LocalPath().c_str());
  if (inspecting)
    local = ui_report.parent_path() / L"gui-state" / ui_report.stem();
  if (store_screenshots) local = store_dir / L"state";
  std::filesystem::path validation_config;
  if (validating) {
    validation_config = argument_text.substr(validation_prefix.size());
    if (!validation_config.is_absolute())
      throw std::runtime_error(
          "Validation configuration must have an absolute path");
    auto bytes = StorageIO::Read(validation_config, 16384);
    auto input = winrt::Windows::Data::Json::JsonObject::Parse(
        winrt::to_hstring(std::string(bytes.begin(), bytes.end())));
    startup_report = input.GetNamedString(L"report").c_str();
    if (!input.GetNamedBoolean(L"enable_hardware"))
      throw std::runtime_error(
          "Hardware validation must be explicitly enabled");
    auto run_id = winrt::to_string(input.GetNamedString(L"run_id"));
    if (run_id.size() != 16 ||
        run_id.find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::runtime_error("Hardware validation run identity is invalid");
    local /= L"system-audio-validation";
    local /= run_id;
    auto base = input.GetNamedNumber(L"base_ms"),
         offset = input.GetNamedNumber(L"offset_ms");
    if (base < SENDER_MIN_PRESENTATION_MS ||
        base > SENDER_MAX_PRESENTATION_MS || offset < -1000 || offset > 1000 ||
        std::floor(base) != base || std::floor(offset) != offset)
      throw std::runtime_error("Hardware initial presentation is invalid");
    SettingsStore initial(local);
    auto saved = initial.Load();
    saved.settings.latency_mode = base == 1000 ? "balanced" : "custom";
    if (input.HasKey(L"latency_mode"))
      saved.settings.latency_mode =
          winrt::to_string(input.GetNamedString(L"latency_mode"));
    saved.settings.custom_ms = static_cast<int>(base);
    ReceiverRecord budget;
    budget.offset_ms = static_cast<int>(offset);
    InitialPresentation(saved.settings, budget);
    ++saved.settings.revision;
    initial.Save(saved);
  }
  startup_operation = "Load WinUI control resources";
  Resources().MergedDictionaries().Append(
      winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources{});
  logger = std::make_shared<Logger>(local / L"logs");
  ui_failure_log = [this](std::string const& message) {
    LogRecord record;
    record.level = LogLevel::warning;
    record.event = LogEvent::command_failed;
    record.detail = message;
    logger->Write(record);
  };
  if (store_screenshots) {
    SettingsStore seed(local);
    auto saved = seed.Load();
    saved.settings.revision += 1;
    saved.settings.language = "zh-CN";
    saved.settings.theme = "light";
    saved.settings.restore_previous = false;
    saved.settings.close_to_tray = false;
    saved.settings.selected_receiver_id.clear();
    saved.receivers.clear();
    seed.Save(saved);
  }
  settings = std::make_unique<SettingsStore>(local);
  credentials = std::make_unique<CredentialStore>(local / L"credentials");
  wchar_t executable[32768]{};
  auto length = GetModuleFileNameW(nullptr, executable, 32768);
  if (!length || length >= 32768) winrt::throw_last_error();
  startup_operation = "Load sender DLL";
  library = std::make_unique<SenderLibrary>(
      std::filesystem::path(executable).parent_path() / L"airplay2_sender.dll");
  startup_operation = "Initialize sender service";
  if (arguments == L"--smoke-window") {
    commands = std::make_unique<CommandDispatcher>(*library);
    auto created = commands->Create().get();
    if (created.status != CommandStatus::applied)
      throw std::runtime_error(created.message);
  } else {
    AudioServices services;
    services.log_sink = [this](LogRecord const& record) {
      logger->Write(record);
    };
    services.log_flush = [writer = logger] { writer->Flush(); };
    services.log_level_changed = [this](LogLevel level) {
      logger->SetLevel(level);
    };
    streaming = std::make_unique<StreamCoordinator>(
        *library, local, !validating && !inspecting && !store_screenshots,
        std::move(services));
  }
  logger->Write(
      {LogLevel::info, LogModule::streaming, LogEvent::command_applied});
  startup_operation = "Create WinUI window";
  window = winrt::Microsoft::UI::Xaml::Window();
  xaml_root = std::filesystem::path(executable).parent_path();
  window.AppWindow().SetIcon(
      (xaml_root / L"Assets" / L"Win2Air.ico").wstring());
  view_model = winrt::make_self<ViewModelBase>(window.DispatcherQueue());
  view_model->PropertyChanged([this](auto const&, auto const& args) {
    if (args.PropertyName() == L"Snapshot") ++snapshot_updates;
  });
  if (commands) view_model->Post(commands->Snapshot());
  if (arguments == L"--smoke-window") {
    auto next = std::make_shared<SessionSnapshot>(*commands->Snapshot());
    ++next->revision;
    std::async(std::launch::async, [this, next] {
      view_model->Post(next);
    }).get();
    view_model->Post(commands->Snapshot());
  }
  window.Title(L"Win2Air");
  if (streaming) {
    startup_operation = "Load playback page and localized resources";
    auto appearance = streaming->Snapshot().preferences;
    ConfigureResources(appearance.language);
    applied_language = appearance.language;
    LoadPlaybackPages();
    window.Content(playback->Content());
    RestorePlan restoration;
    if (initial_activation) {
      auto snapshot = streaming->Snapshot();
      SettingsSnapshot saved;
      saved.settings = snapshot.preferences;
      saved.receivers = snapshot.receivers;
      restoration = PlanRestore(saved, *initial_activation);
      BuildShell(identity_status != APPMODEL_ERROR_NO_PACKAGE);
      tray_model = std::make_unique<TrayViewModel>(*streaming);
      HWND shell_window{};
      winrt::check_hresult(
          window.as<IWindowNative>()->get_WindowHandle(&shell_window));
      power = std::make_unique<PowerNotifications>(
          shell_window, [this](SystemEvent event, unsigned index) noexcept {
            streaming->NotifySystemEvent(event, index);
          });
      auto backend = NativeTrayBackend(
          shell_window,
          [this](TrayEvent event, std::optional<TrayPointer> pointer) {
            tray_click = pointer;
            try {
              LocalizedResources resources;
              tray->Handle(event, streaming->Snapshot(),
                           {resources.GetString(L"TrayOpen").c_str(),
                            resources.GetString(L"SettingsNavigation").c_str(),
                            resources.GetString(L"TrayExit").c_str(),
                            resources.GetString(L"Disconnect").c_str()});
            } catch (winrt::hresult_error const& failure) {
              ShowError(winrt::to_string(failure.message()) +
                        "; HRESULT=" + std::to_string(failure.code().value));
            } catch (std::exception const& failure) {
              ShowError(failure.what());
            }
          });
      tray = std::make_unique<TrayService>(
          std::move(backend), [this](TrayCommand const& command) {
            switch (command.action) {
              case TrayAction::open:
                OpenMainWindow();
                break;
              case TrayAction::settings:
                Navigate(AppPage::settings);
                window.Activate();
                window.AppWindow().Show();
                break;
              case TrayAction::flyout:
                ShowFlyout();
                break;
              case TrayAction::connect:
                shell_pending.push_back(
                    streaming->Connect(command.receiver_id));
                break;
              case TrayAction::disconnect:
                shell_pending.push_back(streaming->Disconnect());
                break;
              case TrayAction::exit:
                RequestClose();
                break;
            }
          });
      tray->Start();
      PrepareFlyout();
      MONITORINFO monitor{sizeof(monitor)};
      if (!GetMonitorInfoW(
              MonitorFromWindow(shell_window, MONITOR_DEFAULTTONEAREST),
              &monitor))
        winrt::throw_last_error();
      auto rect = monitor.rcWork;
      auto position = FitWindow(
          snapshot.preferences.window_placement,
          {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top},
          GetDpiForWindow(shell_window));
      window.AppWindow().MoveAndResize(
          {position.x, position.y, position.width, position.height});
      Navigate(restoration.page);
      if (initial_activation->disconnect)
        shell_pending.push_back(streaming->Disconnect());
      else if (restoration.receiver_id)
        shell_pending.push_back(streaming->Connect(*restoration.receiver_id));
      if (restoration.minimized) window.AppWindow().Hide();
    }
    if (validating)
      audio_validation = std::make_unique<SystemAudioValidation>(
          *streaming, validation_config);
    refresh = winrt::Microsoft::UI::Xaml::DispatcherTimer();
    refresh.Interval(std::chrono::milliseconds(200));
    refresh.Tick([this](auto const&, auto const&) {
      if (!store_screenshots) {
        playback->Refresh();
        if (devices) {
          devices->Refresh();
          if (shell && !FailureDialogBusy())
            if (auto prompt = playback->TakePairingPrompt()) {
              PromptPairing(*prompt);
            }
        }
        TickShell();
      }
      if (shutdown) {
        if (shutdown->wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
          auto result = shutdown->get();
          shutdown.reset();
          if (result.status == CommandStatus::applied || initial_activation) {
            if (result.status != CommandStatus::applied) {
              ShowError(result.message);
              validation_exit = E_FAIL;
            }
            try {
              if (tray) tray->Stop();
            } catch (std::exception const& error) {
              ShowError(error.what());
              validation_exit = E_FAIL;
            }
            allow_close = true;
            flyout_rendered.revoke();
            refresh.Stop();
            if (flyout) flyout.Close();
            window.Close();
            Exit();
          } else
            validation_exit = E_FAIL;
        }
        return;
      }
      if (!ui_report.empty() && std::chrono::steady_clock::now() >= ui_due) {
        if (render_review) {
          if (!review_started) {
            review_started = true;
            CaptureReview();
          }
          return;
        }
        StorageIO::AtomicText(ui_report, playback->LayoutState());
        ui_report.clear();
        RequestClose();
        return;
      }
      if (audio_validation) {
        bool done = false;
        try {
          done = audio_validation->Tick();
        } catch (...) {
          validation_exit = E_FAIL;
          done = true;
        }
        if (done) {
          audio_validation.reset();
          RequestClose();
        }
      }
    });
    refresh.Start();
    if (store_screenshots) CaptureStore();
    window.AppWindow().Closing([this](auto const&, auto const& args) {
      if (!allow_close) {
        args.Cancel(true);
        if (initial_activation &&
            WindowClose(streaming->Snapshot().preferences.close_to_tray,
                        tray && tray->Visible()) == CloseAction::hide) {
          SavePlacement();
          window.AppWindow().Hide();
        } else
          RequestClose();
      }
    });
  }
  if (!initial_activation || !window.AppWindow().IsVisible()) {
    bool hidden =
        initial_activation && (initial_activation->minimized ||
                               (initial_activation->startup &&
                                streaming->Snapshot().preferences.minimized));
    if (!hidden) window.Activate();
  }
  HWND hwnd{};
  winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
  if (!hwnd || (!initial_activation && !IsWindowVisible(hwnd)))
    winrt::throw_hresult(E_UNEXPECTED);
  if (arguments == L"--smoke-window") {
    timer = winrt::Microsoft::UI::Xaml::DispatcherTimer();
    timer.Interval(std::chrono::seconds(3));
    timer.Tick([this](auto const&, auto const&) {
      timer.Stop();
      try {
        if (snapshot_updates != 2 || !view_model->Snapshot())
          winrt::throw_hresult(E_UNEXPECTED);
        LocalizedResources resources;
        if (resources.GetString(L"CommandPending").empty() ||
            resources.GetString(L"CredentialSaveFailed").empty())
          winrt::throw_hresult(E_UNEXPECTED);
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager
            manager;
        auto english_context = manager.CreateResourceContext(),
             chinese_context = manager.CreateResourceContext();
        english_context.QualifierValues().Insert(L"Language", L"en-US");
        chinese_context.QualifierValues().Insert(L"Language", L"zh-CN");
        auto map = manager.MainResourceMap();
        auto english =
                 map.GetValue(L"Resources/CommandPending", english_context)
                     .ValueAsString(),
             chinese =
                 map.GetValue(L"Resources/CommandPending", chinese_context)
                     .ValueAsString();
        if (english.empty() || chinese.empty() || english == chinese)
          winrt::throw_hresult(E_UNEXPECTED);
      } catch (winrt::hresult_error const& error) {
        validation_exit = error.code().value;
      } catch (...) {
        validation_exit = E_FAIL;
      }
      if (commands->Disconnect().get().status != CommandStatus::applied)
        validation_exit = E_FAIL;
      logger->Write({validation_exit ? LogLevel::error : LogLevel::info,
                     LogModule::app,
                     validation_exit ? LogEvent::command_failed
                                     : LogEvent::command_applied,
                     "", 0, validation_exit});
      window.Close();
      Exit();
    });
    timer.Start();
  }
}
}  // namespace Win2Air
