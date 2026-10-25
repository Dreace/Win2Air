#include "DevicesPage.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <cmath>

#include "../Settings/StorageIO.h"
namespace Win2Air {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
namespace Automation = winrt::Microsoft::UI::Xaml::Automation;
namespace {
wchar_t const* StateKey(SessionState state) {
  switch (state) {
    case SessionState::connecting:
      return L"StateConnecting";
    case SessionState::pairing:
      return L"StatePairing";
    case SessionState::setting_up:
      return L"StateSettingUp";
    case SessionState::ready:
      return L"StateReady";
    case SessionState::streaming:
      return L"StateStreaming";
    case SessionState::ending:
      return L"StateEnding";
    case SessionState::recovering:
      return L"StateRecovering";
    case SessionState::error:
      return L"StateError";
    default:
      return L"StateDisconnected";
  }
}
wchar_t const* IndicatorName(ReceiverViewModel const& view) {
  if (!view.active || view.state == SessionState::disconnected)
    return L"IndicatorIdle";
  switch (view.state) {
    case SessionState::ready:
    case SessionState::streaming:
      return L"IndicatorLive";
    case SessionState::error:
      return L"IndicatorFailed";
    default:
      return L"IndicatorBusy";
  }
}
void ShowIndicator(FrameworkElement const& card, wchar_t const* active) {
  for (auto name : {L"IndicatorIdle", L"IndicatorLive", L"IndicatorBusy",
                    L"IndicatorFailed"})
    card.FindName(name).as<UIElement>().Visibility(
        std::wstring_view(name) == active ? Visibility::Visible
                                          : Visibility::Collapsed);
}
std::string CardTitle(std::vector<ReceiverViewModel> const& views,
                      ReceiverViewModel const& view) {
  auto duplicates =
      std::count_if(views.begin(), views.end(), [&](auto const& item) {
        return item.receiver.name == view.receiver.name;
      });
  return duplicates > 1 ? view.receiver.name + " | " + view.receiver.receiver_id
                        : view.receiver.name;
}
}  // namespace
DevicesPage::DevicesPage(
    StreamCoordinator& service, std::filesystem::path const& xaml,
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue)
    : model_(service, queue) {
  auto bytes = StorageIO::Read(xaml, 65536);
  root_ = Markup::XamlReader::Load(
              winrt::to_hstring(std::string(bytes.begin(), bytes.end())))
              .as<FrameworkElement>();
  resources_.ApplyLanguage(root_);
  auto card = StorageIO::Read(xaml.parent_path() / L"ReceiverCard.xaml", 65536);
  card_xaml_.assign(card.begin(), card.end());
  Find<Expander>(L"ManualReceiverOptions")
      .Header(winrt::box_value(resources_.GetString(L"ManualReceiverOptions")));
  Find<CheckBox>(L"ShowHidden")
      .Content(winrt::box_value(resources_.GetString(L"ShowHidden")));
  Find<Button>(L"AddReceiver")
      .Content(winrt::box_value(resources_.GetString(L"AddReceiver")));
  Find<Button>(L"RefreshReceivers")
      .Content(winrt::box_value(resources_.GetString(L"RefreshReceivers")));
  Find<Button>(L"RefreshReceivers").Click([this](auto const&, auto const&) {
    if (!model_.Pending()) model_.RefreshReceivers();
  });
  for (auto pair : {std::pair{L"Host", L"Host"}, std::pair{L"Port", L"Port"}}) {
    Find<TextBlock>((std::wstring(pair.first) + L"Label").c_str())
        .Text(resources_.GetString(pair.second));
    Automation::AutomationProperties::SetName(
        root_.FindName(pair.first).as<DependencyObject>(),
        resources_.GetString(pair.second));
  }
  Find<CheckBox>(L"ShowHidden").Click([this](auto const&, auto const&) {
    Render();
  });
  Find<Button>(L"AddReceiver").Click([this](auto const&, auto const&) {
    auto port = Find<NumberBox>(L"Port").Value();
    if (IsValidReceiverPort(port)) {
      Find<TextBlock>(L"PortError").Text(L"");
      Find<TextBlock>(L"PortError").Visibility(Visibility::Collapsed);
      model_.Add(winrt::to_string(Find<TextBox>(L"Host").Text()),
                 static_cast<unsigned>(port));
    } else {
      Find<TextBlock>(L"PortError").Text(resources_.GetString(L"InvalidPort"));
      Find<TextBlock>(L"PortError").Visibility(Visibility::Visible);
      Find<TextBlock>(L"PortError").StartBringIntoView();
    }
  });
  model_.Projection().PropertyChanged([this](auto const&, auto const& change) {
    if (change.PropertyName() == L"Snapshot") Render();
  });
  Find<FrameworkElement>(L"DeviceScroll")
      .SizeChanged([this](auto const&, auto const&) {
        auto scroll = Find<ScrollViewer>(L"DeviceScroll");
        auto width = scroll.ViewportWidth();
        auto column = Find<FrameworkElement>(L"DeviceColumn");
        if (width > 1 &&
            !(column.Width() > 0 && std::abs(column.Width() - width) <= 1))
          column.Width(width);
      });
  Refresh();
  if (model_.Snapshot().receivers.empty())
    Find<Expander>(L"ManualReceiverOptions").IsExpanded(true);
}
void DevicesPage::Refresh() { model_.Refresh(); }
void DevicesPage::FocusAdd() {
  Find<Expander>(L"ManualReceiverOptions").IsExpanded(true);
  root_.UpdateLayout();
  auto host = Find<TextBox>(L"Host");
  host.Focus(FocusState::Programmatic);
  host.StartBringIntoView();
}
winrt::fire_and_forget DevicesPage::ConfirmReceiverAction(std::string identity,
                                                          bool forget) {
  if (confirmation_open_ || model_.Pending() || FailureDialogBusy()) co_return;
  auto alive = alive_;
  confirmation_open_ = true;
  FailureDialogBusy() = true;
  try {
    std::string name;
    for (auto const& record : model_.Snapshot().receivers)
      if (record.receiver_id == identity) name = record.name;
    ContentDialog dialog;
    resources_.ApplyLanguage(dialog);
    dialog.XamlRoot(root_.XamlRoot());
    dialog.RequestedTheme(root_.RequestedTheme());
    dialog.Title(winrt::box_value(
        resources_.GetString(forget ? L"ReceiverForget" : L"ReceiverDelete")));
    TextBlock content;
    content.Text(winrt::to_hstring(name) + L"\n" +
                 resources_.GetString(forget ? L"ForgetConsequences"
                                             : L"DeleteConsequences"));
    content.TextWrapping(TextWrapping::Wrap);
    dialog.Content(content);
    dialog.PrimaryButtonText(
        resources_.GetString(forget ? L"ReceiverForget" : L"ReceiverDelete"));
    dialog.CloseButtonText(resources_.GetString(L"CancelAction"));
    dialog.DefaultButton(ContentDialogButton::Close);
    auto result = co_await dialog.ShowAsync();
    if (!*alive) {
      FailureDialogBusy() = false;
      co_return;
    }
    if (result == ContentDialogResult::Primary) {
      if (forget)
        model_.Forget(identity);
      else
        model_.Delete(identity);
    }
  } catch (winrt::hresult_error const& error) {
    if (!*alive) {
      FailureDialogBusy() = false;
      co_return;
    }
    model_.InputError(winrt::to_string(error.message()), identity);
  } catch (std::exception const& error) {
    if (*alive) model_.InputError(error.what(), identity);
  }
  if (*alive) confirmation_open_ = false;
  FailureDialogBusy() = false;
}
void DevicesPage::Render() {
  auto const& value = model_.Snapshot();
  Find<Button>(L"RefreshReceivers")
      .IsEnabled(!model_.Pending() && !value.discovery_refreshing);
  auto show_hidden = Find<CheckBox>(L"ShowHidden").IsChecked();
  auto views = ReceiverViews(value, show_hidden && show_hidden.Value());
  Find<TextBlock>(L"Availability")
      .Text(views.empty() ? resources_.GetString(L"NoReceivers") : L"");
  Find<TextBlock>(L"Availability")
      .Visibility(views.empty() ? Visibility::Visible : Visibility::Collapsed);
  RenderCards(views);
  auto const& operation = model_.OperationError();
  ShowBlockingFailure(root_, operation, resources_.GetString(L"DialogClose"),
                      blocking_);
}
void DevicesPage::RenderCards(std::vector<ReceiverViewModel> const& views) {
  auto const& snapshot = model_.Snapshot();
  std::vector<std::string> ids;
  for (auto const& view : views) ids.push_back(view.receiver.receiver_id);
  if (ids != card_ids_) {
    cards_.clear();
    Find<StackPanel>(L"ReceiverCards").Children().Clear();
    for (auto const& view : views) {
      auto card = Markup::XamlReader::Load(winrt::to_hstring(card_xaml_))
                      .as<FrameworkElement>();
      auto id = view.receiver.receiver_id;
      auto weak = winrt::make_weak(card);
      auto text = [&](wchar_t const* name, wchar_t const* key) {
        auto control = card.FindName(name);
        auto label = winrt::box_value(resources_.GetString(key));
        if (auto button = control.try_as<Button>())
          button.Content(label);
        else if (auto check = control.try_as<CheckBox>())
          check.Content(label);
        else if (control.try_as<TextBox>() || control.try_as<NumberBox>() ||
                 control.try_as<Slider>())
          card.FindName(std::wstring(name) + L"Label")
              .as<TextBlock>()
              .Text(resources_.GetString(key));
        else if (auto expander = control.try_as<Expander>())
          expander.Header(label);
        Automation::AutomationProperties::SetName(
            control.as<DependencyObject>(), resources_.GetString(key));
      };
      for (auto name :
           {L"ReceiverDetails", L"ReceiverFavorite", L"ReceiverHidden",
            L"ReceiverAutoConnect", L"ReceiverEditName", L"ReceiverHost",
            L"ReceiverPort", L"ReceiverDelete", L"ReceiverForget",
            L"ReceiverResetLatency"})
        text(name, name);
      text(L"ReceiverNameSave", L"ReceiverSave");
      text(L"ReceiverEndpointSave", L"ReceiverSave");
      card.FindName(L"ReceiverEditName")
          .as<TextBox>()
          .Text(winrt::to_hstring(view.receiver.name));
      auto endpoint = view.receiver.manual_endpoint;
      if (endpoint) {
        card.FindName(L"ReceiverHost")
            .as<TextBox>()
            .Text(winrt::to_hstring(endpoint->host));
        card.FindName(L"ReceiverPort").as<NumberBox>().Value(endpoint->port);
      }
      for (auto name :
           {L"ReceiverFavorite", L"ReceiverHidden", L"ReceiverAutoConnect"})
        card.FindName(name).as<CheckBox>().Click(
            [this, weak, id](auto const&, auto const&) {
              auto card = weak.get();
              if (!card) return;
              for (auto record : model_.Snapshot().receivers)
                if (record.receiver_id == id) {
                  record.favorite = card.FindName(L"ReceiverFavorite")
                                        .as<CheckBox>()
                                        .IsChecked()
                                        .Value();
                  record.hidden = card.FindName(L"ReceiverHidden")
                                      .as<CheckBox>()
                                      .IsChecked()
                                      .Value();
                  record.auto_connect = card.FindName(L"ReceiverAutoConnect")
                                            .as<CheckBox>()
                                            .IsChecked()
                                            .Value();
                  model_.Update(record);
                }
            });
      card.FindName(L"ReceiverNameSave")
          .as<Button>()
          .Click([this, weak, id](auto const&, auto const&) {
            auto card = weak.get();
            if (!card) return;
            for (auto record : model_.Snapshot().receivers)
              if (record.receiver_id == id) {
                record.name = winrt::to_string(
                    card.FindName(L"ReceiverEditName").as<TextBox>().Text());
                model_.Update(record);
              }
          });
      card.FindName(L"ReceiverEndpointSave")
          .as<Button>()
          .Click([this, weak, id](auto const&, auto const&) {
            auto card = weak.get();
            if (!card) return;
            auto port = card.FindName(L"ReceiverPort").as<NumberBox>().Value();
            if (IsValidReceiverPort(port))
              model_.Endpoint(
                  id,
                  winrt::to_string(
                      card.FindName(L"ReceiverHost").as<TextBox>().Text()),
                  static_cast<unsigned>(port));
            else {
              model_.InputError(
                  winrt::to_string(resources_.GetString(L"InvalidPort")), id);
              Render();
            }
          });
      card.FindName(L"ReceiverDelete")
          .as<Button>()
          .Click([this, id](auto const&, auto const&) {
            ConfirmReceiverAction(id, false);
          });
      card.FindName(L"ReceiverForget")
          .as<Button>()
          .Click([this, id](auto const&, auto const&) {
            ConfirmReceiverAction(id, true);
          });
      card.FindName(L"ReceiverResetLatency")
          .as<Button>()
          .Click([this, id](auto const&, auto const&) {
            model_.ResetAdaptiveLatency(id);
          });
      Find<StackPanel>(L"ReceiverCards").Children().Append(card);
      cards_.push_back(card);
    }
    card_ids_ = ids;
  }
  for (size_t index = 0; index < views.size(); ++index) {
    auto const& view = views[index];
    auto card = cards_[index];
    auto title = CardTitle(views, view);
    auto detail = ReceiverDetail(view.receiver);
    card.FindName(L"ReceiverName")
        .as<TextBlock>()
        .Text(winrt::to_hstring(title));
    card.FindName(L"ReceiverDetail")
        .as<TextBlock>()
        .Text(winrt::to_hstring(detail));
    card.FindName(L"ReceiverDetail")
        .as<UIElement>()
        .Visibility(detail.empty() ? Visibility::Collapsed
                                   : Visibility::Visible);
    ShowIndicator(card, IndicatorName(view));
    auto recovery = DeviceFaultText(
        view, winrt::to_string(resources_.GetString(L"AuthenticationRequired")),
        winrt::to_string(resources_.GetString(L"RetryDue")));
    card.FindName(L"ReceiverRecovery")
        .as<TextBlock>()
        .Text(winrt::to_hstring(recovery));
    card.FindName(L"ReceiverRecovery")
        .as<UIElement>()
        .Visibility(recovery.empty() ? Visibility::Collapsed
                                     : Visibility::Visible);
    card.FindName(L"ReceiverEndpointEditor")
        .as<UIElement>()
        .Visibility(view.receiver.manual_endpoint ? Visibility::Visible
                                                  : Visibility::Collapsed);
    card.FindName(L"ReceiverForget")
        .as<UIElement>()
        .Visibility(view.receiver.identity_verified ? Visibility::Visible
                                                    : Visibility::Collapsed);
    card.FindName(L"ReceiverFavorite")
        .as<CheckBox>()
        .IsChecked(view.receiver.favorite);
    card.FindName(L"ReceiverHidden")
        .as<CheckBox>()
        .IsChecked(view.receiver.hidden);
    card.FindName(L"ReceiverAutoConnect")
        .as<CheckBox>()
        .IsChecked(view.receiver.auto_connect);
    card.FindName(L"ReceiverEndpointSave")
        .as<Button>()
        .IsEnabled(!(snapshot.session && snapshot.session->session));
  }
}
}  // namespace Win2Air
