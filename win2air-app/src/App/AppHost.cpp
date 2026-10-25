#include "AppHost.h"

namespace Win2Air {
void AppHost::RequestClose() {
  if (shutdown) return;
  if (receiver_dialog) receiver_dialog.Hide();
  power.reset();
  if (statistics) statistics->CancelExport();
  if (instance) instance->Queue()->Close();
  shutdown.emplace(streaming->Shutdown(true, Placement()));
}
void AppHost::ShowError(std::string const& message) {
  if (logger)
    logger->Write({LogLevel::error,
                   LogModule::app,
                   LogEvent::command_failed,
                   {},
                   0,
                   0,
                   0,
                   0,
                   0,
                   message});
  if (!shell || message.empty()) return;
  LocalizedResources resources;
  ShowBlockingFailure(shell, message, resources.GetString(L"DialogClose"),
                      shell_prompt_);
}
WindowPlacement AppHost::Placement() {
  HWND hwnd{};
  winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
  auto position = window.AppWindow().Position();
  auto size = window.AppWindow().Size();
  auto dpi = GetDpiForWindow(hwnd);
  if (!dpi) winrt::throw_last_error();
  return {position.X, position.Y,
          static_cast<int>(int64_t(size.Width) * 96 / dpi),
          static_cast<int>(int64_t(size.Height) * 96 / dpi), true};
}
void AppHost::PromptAddReceiver() {
  if (FailureDialogBusy()) return;
  HideFlyout();
  window.Activate();
  receiver_dialog = ShowAddReceiverDialog(
      shell,
      [this](std::string host, unsigned port) {
        return streaming->AddReceiver(std::move(host), port);
      },
      [this](std::string message) { ShowError(message); });
}
void AppHost::PromptPairing(std::string identity) {
  if (FailureDialogBusy()) return;
  auto snapshot = streaming->Snapshot();
  auto found = std::find_if(
      snapshot.receivers.begin(), snapshot.receivers.end(),
      [&](auto const& record) { return record.receiver_id == identity; });
  if (found == snapshot.receivers.end()) return;
  HideFlyout();
  window.Activate();
  receiver_dialog = ShowPairingDialog(
      shell, *found,
      [this, identity](SecureBuffer pin) {
        return playback->Pair(identity, std::move(pin));
      },
      [this, identity] {
        playback->DismissPairingPrompt(identity);
        receiver_dialog = nullptr;
      },
      [this](std::string message) { ShowError(message); });
}
void AppHost::ApplyActivation(ActivationRequest const& request, bool show) {
  if (shutdown) {
    ShowError("Application is shutting down; activation rejected");
    return;
  }
  Navigate(request.page);
  if (request.disconnect)
    shell_pending.push_back(streaming->Disconnect());
  else if (request.receiver_id)
    shell_pending.push_back(streaming->Connect(*request.receiver_id));
  if (show && !request.minimized) {
    window.Activate();
    window.AppWindow().Show();
  }
}
void AppHost::OnLaunched(
    winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
  try {
    Launch();
  } catch (winrt::hresult_error const& error) {
    StartupFailed(startup_operation + ": " + winrt::to_string(error.message()) +
                  "; HRESULT=" + std::to_string(error.code().value));
  } catch (std::exception const& error) {
    StartupFailed(startup_operation + ": " + error.what());
  }
}
void AppHost::StartupFailed(std::string const& message) {
  validation_exit = E_FAIL;
  if (!startup_report.empty()) {
    winrt::Windows::Data::Json::JsonObject report;
    report.Insert(
        L"status",
        winrt::Windows::Data::Json::JsonValue::CreateStringValue(L"failed"));
    report.Insert(L"error",
                  winrt::Windows::Data::Json::JsonValue::CreateStringValue(
                      winrt::to_hstring(message)));
    StorageIO::AtomicText(startup_report, winrt::to_string(report.Stringify()));
    Exit();
  } else {
    window = winrt::Microsoft::UI::Xaml::Window();
    window.Title(L"Win2Air");
    winrt::Microsoft::UI::Xaml::Controls::TextBlock detail;
    LogUiFailure(message);
    detail.Text(resource_language.starts_with("zh")
                    ? L"无法启动 Win2Air。"
                    : L"Win2Air could not start.");
    detail.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
    window.Content(detail);
    window.Activate();
  }
}
}  // namespace Win2Air
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
  try {
    std::wstring arguments(command_line ? command_line : L"");
    bool validation = arguments == L"--smoke-window" ||
                      arguments.starts_with(L"--validate-system-audio ") ||
                      arguments.starts_with(L"--validate-playback-ui ") ||
                      arguments.starts_with(L"--review-ui ") ||
                      arguments.starts_with(L"--render-ui ") ||
                      arguments.starts_with(L"--store-screenshots ");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::unique_ptr<Win2Air::NativeInstance> instance;
    if (!validation) {
      instance = std::make_unique<Win2Air::NativeInstance>();
      if (!instance->Primary()) return 0;
    }
    winrt::uninit_apartment();
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    winrt::Microsoft::UI::Xaml::Application::Start(
        [arguments, native = instance.get()](auto const&) {
          winrt::make<Win2Air::AppHost>(arguments, native);
        });
    return Win2Air::validation_exit;
  } catch (winrt::hresult_error const& error) {
    OutputDebugStringW(error.message().c_str());
    Win2Air::LogUiFailure(winrt::to_string(error.message()));
    MessageBoxW(nullptr, L"Win2Air could not start.", L"Win2Air",
                MB_OK | MB_ICONERROR);
    return static_cast<int>(error.code().value);
  } catch (std::exception const& error) {
    OutputDebugStringA(error.what());
    Win2Air::LogUiFailure(error.what());
    MessageBoxW(nullptr, L"Win2Air could not start.", L"Win2Air",
                MB_OK | MB_ICONERROR);
    return static_cast<int>(E_INVALIDARG);
  } catch (...) {
    OutputDebugStringW(
        L"Win2Air startup failed with an unexpected native exception.");
    return static_cast<int>(E_UNEXPECTED);
  }
}
