#include "PlaybackPage.h"
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

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
PlaybackPage::PlaybackPage(
    StreamCoordinator& service, std::filesystem::path const& xaml,
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue)
    : model_(service, queue) {
  auto bytes = StorageIO::Read(xaml, 65536);
  root_ = Markup::XamlReader::Load(
              winrt::to_hstring(std::string(bytes.begin(), bytes.end())))
              .as<FrameworkElement>();
  resources_.ApplyLanguage(root_);
  auto card =
      StorageIO::Read(xaml.parent_path() / L"SessionDeviceCard.xaml", 65536);
  card_xaml_.assign(card.begin(), card.end());
  for (auto pair : {std::pair{L"VolumeHeading", L"ReceiverVolume"},
                    {L"SourceHeading", L"AudioSource"}})
    Find<TextBlock>(pair.first).Text(resources_.GetString(pair.second));
  for (auto pair : {std::pair{L"AddDevice", L"AddDevice"},
                    {L"RefreshReceivers", L"RefreshReceivers"},
                    {L"OpenAudioSettings", L"AudioSettingsAction"},
                    {L"CurrentConnect", L"Connect"}})
    Find<Button>(pair.first)
        .Content(winrt::box_value(resources_.GetString(pair.second)));
  Find<Button>(L"CurrentDisconnect")
      .Content(winrt::box_value(resources_.GetString(L"Disconnect")));
  Find<Button>(L"CurrentConnect").Style(Find<Button>(L"AccentSample").Style());
  Find<FrameworkElement>(L"PlaybackScroll")
      .SizeChanged([this](auto const&, auto const&) {
        auto scroll = Find<ScrollViewer>(L"PlaybackScroll");
        auto width = scroll.ViewportWidth();
        auto column = Find<FrameworkElement>(L"PlaybackColumn");
        if (width > 1 &&
            !(column.Width() > 0 && std::abs(column.Width() - width) <= 1))
          column.Width(width);
      });
  Find<Grid>(L"CurrentIdentity").SizeChanged([this](auto const&, auto const&) {
    auto row = Find<Grid>(L"CurrentIdentity");
    auto status = Find<FrameworkElement>(L"CurrentStatus");
    auto name = Find<TextBlock>(L"CurrentName");
    auto limit =
        row.ActualWidth() - status.ActualWidth() - row.ColumnSpacing() * 2;
    if (limit < 48) return;
    if (!(name.MaxWidth() > 0) || std::abs(name.MaxWidth() - limit) > 1)
      name.MaxWidth(limit);
  });
  Automation::AutomationProperties::SetName(
      Find<Slider>(L"DestinationVolume"),
      resources_.GetString(L"ReceiverVolume"));
  Automation::AutomationProperties::SetName(
      Find<ComboBox>(L"SourceSummary"), resources_.GetString(L"AudioSource"));
  Find<Button>(L"AddDevice").Click([this](auto const&, auto const&) {
    if (open_devices_) open_devices_();
  });
  Find<Button>(L"RefreshReceivers").Click([this](auto const&, auto const&) {
    if (!model_.Pending()) model_.RefreshReceivers();
  });
  Find<Button>(L"OpenAudioSettings").Click([this](auto const&, auto const&) {
    if (open_audio_) open_audio_();
  });
  Find<ComboBox>(L"SourceSummary")
      .SelectionChanged([this](auto const&, auto const&) {
        if (!refreshing_ && !model_.Pending()) {
          auto index = Find<ComboBox>(L"SourceSummary").SelectedIndex();
          if (index >= 0) model_.Source(index);
        }
      });
  Find<Button>(L"CurrentDisconnect").Click([this](auto const&, auto const&) {
    if (!model_.Pending()) model_.Disconnect();
  });
  Find<Button>(L"CurrentConnect").Click([this](auto const&, auto const&) {
    if (!model_.Pending() && !selected_id_.empty())
      model_.ConnectIdentity(selected_id_);
  });
  Find<Slider>(L"DestinationVolume")
      .ValueChanged([this](auto const&, auto const&) {
        if (!refreshing_ && !selected_id_.empty())
          model_.Volume(selected_id_,
                        Find<Slider>(L"DestinationVolume").Value() / 100);
      });
  Find<Primitives::ToggleButton>(L"DestinationMuted")
      .Click([this](auto const&, auto const&) {
        if (!refreshing_ && !selected_id_.empty())
          model_.Mute(selected_id_,
                      Find<Primitives::ToggleButton>(L"DestinationMuted")
                          .IsChecked()
                          .Value());
      });
  model_.Projection().PropertyChanged([this](auto const&, auto const& change) {
    if (change.PropertyName() == L"Snapshot") Render();
  });
  Refresh();
}
void PlaybackPage::Refresh() { model_.Refresh(); }
void PlaybackPage::Present(PlaybackSnapshot snapshot) {
  model_.Present(std::move(snapshot));
  Render();
}
std::optional<std::string> PlaybackPage::TakePairingPrompt() {
  auto prompt = std::move(pending_pairing_);
  pending_pairing_.reset();
  return prompt;
}
bool PlaybackPage::SessionOwnsSelection() const {
  auto const& value = model_.Snapshot();
  return value.session && value.session->session &&
         value.session->intent.desired_connected &&
         value.session->session->state != SessionState::error;
}
void PlaybackPage::ResolveSelection(
    std::vector<ReceiverViewModel> const& views) {
  if (auto selected = model_.Selection(views))
    selected_id_ = *selected >= 0
                       ? views[size_t(*selected)].receiver.receiver_id
                       : std::string{};
  if (SessionOwnsSelection()) {
    selected_id_ = model_.Snapshot().session->session->receiver_id;
    return;
  }
  auto found = std::find_if(views.begin(), views.end(), [&](auto const& view) {
    return view.receiver.receiver_id == selected_id_;
  });
  if (found == views.end())
    selected_id_ =
        views.empty() ? std::string{} : views.front().receiver.receiver_id;
}
std::string PlaybackPage::LayoutState() const {
  using namespace winrt::Windows::Data::Json;
  JsonObject state;
  state.Insert(L"text_language",
               JsonValue::CreateStringValue(root_.Language()));
  state.Insert(L"loaded", JsonValue::CreateBooleanValue(root_.IsLoaded()));
  state.Insert(L"width", JsonValue::CreateNumberValue(root_.ActualWidth()));
  state.Insert(L"height", JsonValue::CreateNumberValue(root_.ActualHeight()));
  state.Insert(L"media_title", JsonValue::CreateStringValue(
                                   Find<TextBlock>(L"NowPlayingTitle").Text()));
  state.Insert(L"media_details",
               JsonValue::CreateStringValue(
                   Find<TextBlock>(L"NowPlayingDetails").Text()));
  state.Insert(L"artwork_loaded",
               JsonValue::CreateBooleanValue(
                   bool(Find<Image>(L"NowPlayingArtwork").Source())));
  state.Insert(L"children",
               JsonValue::CreateNumberValue(
                   Media::VisualTreeHelper::GetChildrenCount(root_)));
  for (auto name : {L"SourceHeading", L"SourceSummary", L"DeviceCards",
                    L"DestinationVolume", L"Status", L"NowPlayingBar"}) {
    auto element = Find<FrameworkElement>(name);
    JsonObject item;
    item.Insert(L"loaded", JsonValue::CreateBooleanValue(element.IsLoaded()));
    item.Insert(L"width", JsonValue::CreateNumberValue(element.ActualWidth()));
    item.Insert(L"height",
                JsonValue::CreateNumberValue(element.ActualHeight()));
    item.Insert(L"visible",
                JsonValue::CreateBooleanValue(
                    element.Visibility() == Visibility::Visible &&
                    element.ActualWidth() > 0 && element.ActualHeight() > 0));
    state.Insert(name, item);
  }
  return winrt::to_string(state.Stringify());
}
}  // namespace Win2Air
