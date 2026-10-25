#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include "PlaybackPage.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

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
namespace {
winrt::fire_and_forget ShowArtwork(
    winrt::Microsoft::UI::Xaml::Controls::Image image,
    std::shared_ptr<NowPlaying const> media) {
  using namespace winrt::Windows::Storage::Streams;
  using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
  auto revision = winrt::to_hstring(media->revision);
  try {
    InMemoryRandomAccessStream stream;
    DataWriter writer;
    writer.WriteBytes(media->artwork);
    co_await stream.WriteAsync(writer.DetachBuffer());
    stream.Seek(0);
    BitmapImage bitmap;
    bitmap.DecodePixelWidth(256);
    co_await bitmap.SetSourceAsync(stream);
    if (winrt::unbox_value<winrt::hstring>(image.Tag()) == revision)
      image.Source(bitmap);
  } catch (winrt::hresult_error const& error) {
    LogUiFailure(winrt::to_string(error.message()));
  }
}
}  // namespace
void PlaybackPage::Render() {
  auto const& value = model_.Snapshot();
  auto media = value.now_playing;
  if (media && media_revision_ != media->revision) {
    media_revision_ = media->revision;
    auto artwork = Find<Image>(L"NowPlayingArtwork");
    artwork.Tag(winrt::box_value(winrt::to_hstring(media->revision)));
    artwork.Source(nullptr);
    Find<Border>(L"NowPlayingArtworkFrame")
        .Visibility(media->artwork.empty() ? Visibility::Collapsed
                                           : Visibility::Visible);
    Find<TextBlock>(L"NowPlayingTitle").Text(winrt::to_hstring(media->title));
    auto details = media->artist;
    if (!media->album.empty()) {
      if (!details.empty()) details += " | ";
      details += media->album;
    }
    Find<TextBlock>(L"NowPlayingDetails").Text(winrt::to_hstring(details));
    Find<TextBlock>(L"NowPlayingDetails")
        .Visibility(details.empty() ? Visibility::Collapsed
                                    : Visibility::Visible);
    if (!media->artwork.empty()) ShowArtwork(artwork, media);
  }
  Find<Border>(L"NowPlayingBar")
      .Visibility(media && (!media->title.empty() || !media->artwork.empty())
                      ? Visibility::Visible
                      : Visibility::Collapsed);
  if (value.metadata_error != logged_metadata_error_) {
    LogUiFailure(value.metadata_error);
    logged_metadata_error_ = value.metadata_error;
  }
  if (value.discovery_error != logged_discovery_error_) {
    LogUiFailure(value.discovery_error);
    logged_discovery_error_ = value.discovery_error;
  }
  refreshing_ = true;
  auto views = ReceiverViews(value);
  auto selected_source = std::find_if(
      value.sources.begin(), value.sources.end(), [&](auto const& item) {
        return item.id == value.source_config.endpoint_id;
      });
  auto source = Find<ComboBox>(L"SourceSummary");
  std::vector<std::string> source_names;
  for (auto const& item : value.sources)
    source_names.push_back(item.id + "\n" + item.name);
  if (source_names != source_names_ || source.Items().Size() == 0) {
    source_names_ = std::move(source_names);
    source.Items().Clear();
    source.Items().Append(
        winrt::box_value(resources_.GetString(L"FollowDefault")));
    for (auto const& item : value.sources)
      source.Items().Append(winrt::box_value(winrt::to_hstring(item.name)));
  }
  source.SelectedIndex(value.source_config.follow_system_default ? 0
                       : selected_source == value.sources.end()
                           ? -1
                           : int(selected_source - value.sources.begin()) + 1);
  source.IsEnabled(!model_.Pending());
  auto source_state = Find<TextBlock>(L"SourceAvailability");
  bool missing_source =
      value.sources.empty() || (!value.source_config.follow_system_default &&
                                selected_source == value.sources.end());
  source_state.Text(missing_source ? resources_.GetString(L"SourceUnavailable")
                                   : L"");
  source_state.Visibility(missing_source ? Visibility::Visible
                                         : Visibility::Collapsed);
  ResolveSelection(views);
  auto add = Find<Button>(L"AddDevice");
  Find<Button>(L"RefreshReceivers")
      .IsEnabled(!model_.Pending() && !value.discovery_refreshing);
  if (views.empty())
    add.Style(Find<Button>(L"AccentSample").Style());
  else
    add.ClearValue(FrameworkElement::StyleProperty());
  Find<TextBlock>(L"Availability")
      .Text(views.empty() ? resources_.GetString(L"NoReceivers") : L"");
  Find<TextBlock>(L"Availability")
      .Visibility(views.empty() ? Visibility::Visible : Visibility::Collapsed);
  auto current = std::find_if(views.begin(), views.end(),
                              [](auto const& view) { return view.active; });
  bool has_current = current != views.end();
  Find<FrameworkElement>(L"CurrentVolumeRow")
      .Visibility(has_current ? Visibility::Visible : Visibility::Collapsed);
  Find<TextBlock>(L"CurrentName").FontSize(has_current ? 27 : 20);
  Find<TextBlock>(L"CurrentName")
      .Text(has_current ? winrt::to_hstring(CardTitle(views, *current))
                        : resources_.GetString(L"NoActiveReceiver"));
  auto current_detail =
      has_current ? winrt::to_hstring(ReceiverDetail(current->receiver))
                  : winrt::hstring{};
  Find<TextBlock>(L"CurrentDetail").Text(current_detail);
  Find<TextBlock>(L"CurrentDetail")
      .Visibility(current_detail.empty() ? Visibility::Collapsed
                                         : Visibility::Visible);
  Automation::AutomationProperties::SetName(
      Find<TextBlock>(L"CurrentName"), Find<TextBlock>(L"CurrentName").Text());
  ToolTipService::SetToolTip(
      Find<TextBlock>(L"CurrentName"),
      winrt::box_value(Find<TextBlock>(L"CurrentName").Text()));
  Find<TextBlock>(L"DestinationHeading")
      .Text(resources_.GetString(has_current ? L"SwitchReceiverHeading"
                                             : L"AvailableReceiversHeading"));
  Find<Button>(L"CurrentDisconnect")
      .Visibility(has_current ? Visibility::Visible : Visibility::Collapsed);
  Find<Button>(L"CurrentDisconnect").IsEnabled(!model_.Pending());
  Find<Button>(L"CurrentConnect")
      .Visibility(has_current && current->state == SessionState::error
                      ? Visibility::Visible
                      : Visibility::Collapsed);
  Find<Button>(L"CurrentConnect").IsEnabled(!model_.Pending());
  RenderCards(views);

  ShowDestinationVolume();
  auto stage = value.session && value.session->session
                   ? value.session->session->state
                   : SessionState::disconnected;
  auto standby = value.session && value.session->session
                     ? value.session->session->standby_state
                     : StandbyState::active;
  auto active = value.session && value.session->session.has_value();
  bool authentication =
      active && value.session->session->failure_kind == "authentication";
  auto connected = has_current ? ConnectedStatusKey(stage, standby) : nullptr;
  auto status_key = authentication ? L"AuthenticationRequired"
                    : connected    ? connected
                                   : StateKey(stage);
  Find<TextBlock>(L"Status").Text(resources_.GetString(status_key));
  bool playing = has_current && (stage == SessionState::ready ||
                                 stage == SessionState::streaming);
  Find<TextBlock>(L"Status").Foreground(
      playing ? Find<Shapes::Ellipse>(L"CurrentStatusIndicator").Fill()
              : Find<TextBlock>(L"SourceHeading").Foreground());
  Find<Shapes::Ellipse>(L"CurrentStatusIndicator")
      .Visibility(playing ? Visibility::Visible : Visibility::Collapsed);
  auto authentication_text =
      winrt::to_string(resources_.GetString(L"AuthenticationRequired"));
  auto retry_text = winrt::to_string(resources_.GetString(L"RetryDue"));
  auto current_fault =
      has_current ? DeviceFaultText(*current, authentication_text, retry_text)
                  : std::string{};
  Find<TextBlock>(L"CurrentFault").Text(winrt::to_hstring(current_fault));
  Find<TextBlock>(L"CurrentFault")
      .Visibility(current_fault.empty() ? Visibility::Collapsed
                                        : Visibility::Visible);
  bool session_on_card = false;
  if (value.session && value.session->session) {
    auto const& session_id = value.session->session->receiver_id;
    session_on_card =
        std::any_of(views.begin(), views.end(), [&](auto const& view) {
          return view.receiver.receiver_id == session_id;
        });
  }
  auto alert = model_.BlockingError();
  auto add_alert = [&](std::string const& part) {
    if (part.empty() || alert.find(part) != std::string::npos) return;
    if (!alert.empty()) alert += '\n';
    alert += part;
  };
  add_alert(model_.OperationError());
  if (!session_on_card) {
    add_alert(value.adaptive_error);
    add_alert(value.credential_error);
  }
  ShowBlockingFailure(root_, alert, resources_.GetString(L"DialogClose"),
                      blocking_);
  if (auto prompt = pairing_prompt_.Observe(value.session))
    pending_pairing_ = std::move(prompt);
  refreshing_ = false;
}
void PlaybackPage::ShowDestinationVolume() {
  double volume = 0;
  bool enabled = false;
  bool muted = false;
  bool chosen = false;
  if (!selected_id_.empty()) {
    for (auto const& view : ReceiverViews(model_.Snapshot())) {
      if (view.receiver.receiver_id != selected_id_) continue;
      volume = view.receiver.volume;
      muted = view.receiver.muted;
      enabled = view.active && view.adjustable;
      chosen = view.active;
      break;
    }
  }
  auto previous = refreshing_;
  refreshing_ = true;
  Find<Slider>(L"DestinationVolume").Value(volume * 100);
  Find<Slider>(L"DestinationVolume").IsEnabled(enabled);
  Find<Primitives::ToggleButton>(L"DestinationMuted").IsChecked(muted);
  Find<Primitives::ToggleButton>(L"DestinationMuted").IsEnabled(enabled);
  Find<FontIcon>(L"MuteIcon").Glyph(muted ? L"\uE198" : L"\uE15D");
  Automation::AutomationProperties::SetName(
      Find<Primitives::ToggleButton>(L"DestinationMuted"),
      resources_.GetString(muted ? L"UnmuteAction" : L"MuteAction"));
  ToolTipService::SetToolTip(
      Find<Primitives::ToggleButton>(L"DestinationMuted"),
      winrt::box_value(
          resources_.GetString(muted ? L"UnmuteAction" : L"MuteAction")));
  Find<TextBlock>(L"DestinationVolumeValue")
      .Text(chosen
                ? std::to_wstring(muted ? 0 : std::lround(volume * 100)) + L"%"
                : L"");
  refreshing_ = previous;
}
void PlaybackPage::RenderCards(
    std::vector<ReceiverViewModel> const& all_views) {
  std::vector<ReceiverViewModel> views;
  for (auto const& view : all_views)
    if (!view.active) views.push_back(view);
  std::vector<std::string> ids;
  for (auto const& view : views) ids.push_back(view.receiver.receiver_id);
  Find<FrameworkElement>(L"CandidateList")
      .Visibility(views.empty() ? Visibility::Collapsed : Visibility::Visible);
  auto accent = Find<Button>(L"AccentSample").Style();
  if (ids != card_ids_) {
    cards_.clear();
    Find<StackPanel>(L"DeviceCards").Children().Clear();
    for (auto const& view : views) {
      auto card = Markup::XamlReader::Load(winrt::to_hstring(card_xaml_))
                      .as<FrameworkElement>();
      auto id = view.receiver.receiver_id;
      card.FindName(L"SessionHeading")
          .as<UIElement>()
          .Tapped([this, id](auto const&, auto const&) {
            if (refreshing_ || SessionOwnsSelection() || selected_id_ == id)
              return;
            selected_id_ = id;
            Render();
          });
      for (auto name : {L"SessionConnect", L"SessionDisconnect"}) {
        auto button = card.FindName(name).as<Button>();
        button.GotFocus([this, id](auto const&, auto const&) {
          if (refreshing_ || SessionOwnsSelection() || selected_id_ == id)
            return;
          selected_id_ = id;
          Render();
        });
      }
      card.FindName(L"SessionConnect")
          .as<Button>()
          .Click([this, id](auto const&, auto const&) {
            if (model_.Pending()) return;
            selected_id_ = id;
            model_.ConnectIdentity(id);
            Render();
          });
      card.FindName(L"SessionDisconnect")
          .as<Button>()
          .Click([this](auto const&, auto const&) {
            if (!model_.Pending()) model_.Disconnect();
          });
      Find<StackPanel>(L"DeviceCards").Children().Append(card);
      cards_.push_back(card);
    }
    card_ids_ = ids;
  }
  for (size_t index = 0; index < views.size(); ++index) {
    auto const& view = views[index];
    auto card = cards_[index];
    auto title = CardTitle(views, view);
    auto detail = ReceiverDetail(view.receiver);
    bool selected = view.receiver.receiver_id == selected_id_;
    bool failed = view.active && view.state == SessionState::error;
    bool show_connect = !view.active || failed;
    bool show_disconnect = view.active;
    card.FindName(L"SessionName")
        .as<TextBlock>()
        .Text(winrt::to_hstring(title));
    card.FindName(L"SessionDetail")
        .as<TextBlock>()
        .Text(winrt::to_hstring(detail));
    card.FindName(L"SessionDetail")
        .as<UIElement>()
        .Visibility(detail.empty() ? Visibility::Collapsed
                                   : Visibility::Visible);
    auto fault = DeviceFaultText(
        view, winrt::to_string(resources_.GetString(L"AuthenticationRequired")),
        winrt::to_string(resources_.GetString(L"RetryDue")));
    card.FindName(L"SessionFault")
        .as<TextBlock>()
        .Text(winrt::to_hstring(fault));
    card.FindName(L"SessionFault")
        .as<UIElement>()
        .Visibility(fault.empty() ? Visibility::Collapsed
                                  : Visibility::Visible);
    ShowIndicator(card, IndicatorName(view));
    auto state = resources_.GetString(view.active ? StateKey(view.state)
                                                  : L"ReceiverInactive");
    card.FindName(L"SessionStatus").as<TextBlock>().Text(state);
    ToolTipService::SetToolTip(
        card.FindName(L"SessionName").as<FrameworkElement>(),
        winrt::box_value(winrt::to_hstring(title)));
    Automation::AutomationProperties::SetName(
        card, winrt::to_hstring(title) + L", " + winrt::to_hstring(detail) +
                  L", " + state);
    auto connect = card.FindName(L"SessionConnect").as<Button>();
    auto disconnect = card.FindName(L"SessionDisconnect").as<Button>();
    connect.Content(winrt::box_value(resources_.GetString(L"Connect")));
    disconnect.Content(winrt::box_value(resources_.GetString(L"Disconnect")));
    Automation::AutomationProperties::SetAutomationId(connect, L"Connect");
    Automation::AutomationProperties::SetAutomationId(disconnect,
                                                      L"Disconnect");
    Automation::AutomationProperties::SetName(
        connect,
        resources_.GetString(L"Connect") + L" " + winrt::to_hstring(title));
    Automation::AutomationProperties::SetName(
        disconnect,
        resources_.GetString(L"Disconnect") + L" " + winrt::to_hstring(title));
    connect.Visibility(show_connect ? Visibility::Visible
                                    : Visibility::Collapsed);
    disconnect.Visibility(show_disconnect ? Visibility::Visible
                                          : Visibility::Collapsed);
    connect.IsEnabled(show_connect && !model_.Pending());
    disconnect.IsEnabled(show_disconnect && !model_.Pending());
    card.FindName(L"SessionCard")
        .as<Border>()
        .BorderThickness({0, 0, 0, index + 1 == views.size() ? 0.0 : 1.0});
    if (show_connect && selected)
      connect.Style(accent);
    else
      connect.ClearValue(FrameworkElement::StyleProperty());
  }
}
}  // namespace Win2Air
