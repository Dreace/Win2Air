#include "ReceiverDialogs.h"
#undef GetClassName
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include "LocalizedResources.h"
#include "ViewModels/ReceiverViewModel.h"

namespace Win2Air {
namespace {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
struct InputState {
  std::optional<std::future<CommandResult>> pending;
  bool open{true};
};
winrt::fire_and_forget ShowReceiverInput(
    FrameworkElement owner, ContentDialog dialog,
    std::optional<ReceiverRecord> receiver, PairingSubmit pair,
    AddReceiverSubmit add, std::function<void()> closed,
    std::function<void(std::string)> failed) {
  if (!owner || !owner.XamlRoot() || FailureDialogBusy()) co_return;
  auto state = std::make_shared<InputState>();
  DispatcherTimer poll;
  FailureDialogBusy() = true;
  PasswordBox pin{nullptr};
  std::string failure_message;
  try {
    LocalizedResources resources;
    resources.ApplyLanguage(dialog);
    dialog.XamlRoot(owner.XamlRoot());
    dialog.RequestedTheme(owner.RequestedTheme());
    dialog.Title(winrt::box_value(
        resources.GetString(receiver ? L"PairingDialogTitle" : L"AddDevice")));
    dialog.PrimaryButtonText(
        resources.GetString(receiver ? L"Connect" : L"AddReceiver"));
    dialog.SecondaryButtonText(resources.GetString(L"CancelAction"));
    dialog.DefaultButton(ContentDialogButton::Primary);
    StackPanel content;
    content.Spacing(12);
    content.MaxWidth(420);
    auto error = Markup::XamlReader::Load(
                     L"<TextBlock "
                     L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/"
                     L"presentation' "
                     L"TextWrapping='Wrap' Foreground='{ThemeResource "
                     L"AirErrorBrush}' />")
                     .as<TextBlock>();
    error.Visibility(Visibility::Collapsed);
    error.SetValue(
        Automation::AutomationProperties::LiveSettingProperty(),
        winrt::box_value(Automation::Peers::AutomationLiveSetting::Polite));
    TextBox host{nullptr};
    NumberBox port{nullptr};
    PasswordBox::PasswordChanged_revoker password_changed;
    if (receiver) {
      TextBlock name;
      name.Text(winrt::to_hstring(receiver->name));
      name.TextWrapping(TextWrapping::Wrap);
      content.Children().Append(name);
      TextBlock instruction;
      instruction.Text(resources.GetString(L"PairingDialogHint"));
      instruction.TextWrapping(TextWrapping::Wrap);
      content.Children().Append(instruction);
      pin = PasswordBox();
      pin.Header(winrt::box_value(resources.GetString(L"PairingCode")));
      pin.MaxLength(128);
      Automation::AutomationProperties::SetAutomationId(pin, L"ReceiverPin");
      content.Children().Append(pin);
      dialog.IsPrimaryButtonEnabled(false);
      password_changed = pin.PasswordChanged(
          winrt::auto_revoke, [state, dialog, pin](auto const&, auto const&) {
            if (!state->pending)
              dialog.IsPrimaryButtonEnabled(!pin.Password().empty());
          });
    } else {
      host = TextBox();
      host.Header(winrt::box_value(resources.GetString(L"Host")));
      host.MaxLength(253);
      Automation::AutomationProperties::SetAutomationId(host, L"ReceiverHost");
      port = NumberBox();
      port.Header(winrt::box_value(resources.GetString(L"Port")));
      port.Minimum(1);
      port.Maximum(65535);
      port.Value(7000);
      Automation::AutomationProperties::SetAutomationId(port, L"ReceiverPort");
      content.Children().Append(host);
      content.Children().Append(port);
    }
    ProgressRing busy;
    busy.IsActive(false);
    busy.Visibility(Visibility::Collapsed);
    Automation::AutomationProperties::SetName(
        busy, resources.GetString(L"StateConnecting"));
    content.Children().Append(busy);
    content.Children().Append(error);
    dialog.Content(content);
    auto opened = dialog.Opened(winrt::auto_revoke,
                                [pin, host](auto const&, auto const&) {
                                  if (pin)
                                    pin.Focus(FocusState::Programmatic);
                                  else
                                    host.Focus(FocusState::Programmatic);
                                });
    auto closing = dialog.Closing(
        winrt::auto_revoke,
        [state](auto const&, ContentDialogClosingEventArgs const& args) {
          if (state->pending) args.Cancel(true);
        });
    auto clicked = dialog.PrimaryButtonClick(
        winrt::auto_revoke,
        [state, dialog, pin, host, port, busy, error, pair, add, resources](
            auto const&, ContentDialogButtonClickEventArgs const& args) {
          args.Cancel(true);
          if (state->pending) return;
          error.Visibility(Visibility::Collapsed);
          try {
            if (pin) {
              if (pin.Password().empty()) return;
              auto text = winrt::to_string(pin.Password());
              SecureBuffer secret;
              try {
                secret = SecureBuffer(text);
              } catch (...) {
                SecureZeroMemory(text.data(), text.size());
                throw;
              }
              SecureZeroMemory(text.data(), text.size());
              pin.Password(L"");
              state->pending.emplace(pair(std::move(secret)));
            } else {
              auto address = winrt::to_string(host.Text());
              if (address.empty() ||
                  address.find_first_not_of(" \t\r\n") == std::string::npos) {
                error.Text(resources.GetString(L"ReceiverHostRequired"));
                error.Visibility(Visibility::Visible);
                host.Focus(FocusState::Programmatic);
                return;
              }
              if (!IsValidReceiverPort(port.Value())) {
                error.Text(resources.GetString(L"InvalidPort"));
                error.Visibility(Visibility::Visible);
                port.Focus(FocusState::Programmatic);
                return;
              }
              state->pending.emplace(
                  add(std::move(address), static_cast<unsigned>(port.Value())));
            }
            dialog.IsPrimaryButtonEnabled(false);
            dialog.IsSecondaryButtonEnabled(false);
            if (pin) pin.IsEnabled(false);
            if (host) {
              host.IsEnabled(false);
              port.IsEnabled(false);
            }
            busy.Visibility(Visibility::Visible);
            busy.IsActive(true);
          } catch (std::exception const& failure) {
            LogUiFailure(failure.what());
            error.Text(resources.GetString(pin ? L"PairingDialogFailed"
                                               : L"AddReceiverDialogFailed"));
            error.Visibility(Visibility::Visible);
          } catch (winrt::hresult_error const& failure) {
            LogUiFailure(winrt::to_string(failure.message()));
            error.Text(resources.GetString(pin ? L"PairingDialogFailed"
                                               : L"AddReceiverDialogFailed"));
            error.Visibility(Visibility::Visible);
          }
        });
    poll.Interval(std::chrono::milliseconds(100));
    auto tick = poll.Tick(
        winrt::auto_revoke, [state, dialog, pin, host, port, busy, error,
                             resources](auto const&, auto const&) {
          if (!state->open || !state->pending ||
              state->pending->wait_for(std::chrono::seconds(0)) !=
                  std::future_status::ready)
            return;
          CommandResult result;
          try {
            result = state->pending->get();
          } catch (winrt::hresult_error const& failure) {
            result.status = CommandStatus::failed;
            result.message = winrt::to_string(failure.message());
          } catch (std::exception const& failure) {
            result.status = CommandStatus::failed;
            result.message = failure.what();
          }
          state->pending.reset();
          if (result.status == CommandStatus::applied) {
            dialog.Hide();
            return;
          }
          busy.IsActive(false);
          busy.Visibility(Visibility::Collapsed);
          error.Text(resources.GetString(pin ? L"PairingDialogFailed"
                                             : L"AddReceiverDialogFailed"));
          error.Visibility(Visibility::Visible);
          LogUiFailure(result.message);
          dialog.IsSecondaryButtonEnabled(true);
          dialog.IsPrimaryButtonEnabled(!pin);
          if (pin) {
            pin.IsEnabled(true);
            pin.Focus(FocusState::Programmatic);
          }
          if (host) {
            host.IsEnabled(true);
            port.IsEnabled(true);
          }
        });
    poll.Start();
    co_await dialog.ShowAsync();
  } catch (winrt::hresult_error const& failure) {
    failure_message = winrt::to_string(failure.message());
  } catch (std::exception const& failure) {
    failure_message = failure.what();
  }
  state->open = false;
  poll.Stop();
  if (pin) pin.Password(L"");
  if (closed) closed();
  FailureDialogBusy() = false;
  if (!failure_message.empty() && failed) failed(std::move(failure_message));
}
}  // namespace
winrt::Microsoft::UI::Xaml::Controls::ContentDialog ShowPairingDialog(
    winrt::Microsoft::UI::Xaml::FrameworkElement owner, ReceiverRecord receiver,
    PairingSubmit submit, std::function<void()> closed,
    std::function<void(std::string)> failed) {
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  ShowReceiverInput(std::move(owner), dialog, std::move(receiver),
                    std::move(submit), {}, std::move(closed),
                    std::move(failed));
  return dialog;
}
winrt::Microsoft::UI::Xaml::Controls::ContentDialog ShowAddReceiverDialog(
    winrt::Microsoft::UI::Xaml::FrameworkElement owner,
    AddReceiverSubmit submit, std::function<void(std::string)> failed) {
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog;
  ShowReceiverInput(std::move(owner), dialog, {}, {}, std::move(submit), {},
                    std::move(failed));
  return dialog;
}
}  // namespace Win2Air
