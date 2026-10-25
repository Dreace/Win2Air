#include "SettingsPage.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <tuple>
#include "../App/OpenSourceNotices.h"
#include "../Settings/StorageIO.h"
#include "../Streaming/FailureDetail.h"
namespace Win2Air {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
void SettingsPage::Input(std::function<void()> action) {
  try {
    action();
    input_error_.clear();
  } catch (...) {
    input_error_ = DescribeFailure().message;
  }
}
SettingsPage::SettingsPage(StreamCoordinator& service,
                           std::filesystem::path const& path,
                           std::shared_ptr<SettingsDraft> draft)
    : model_(service, std::move(draft)) {
  auto bytes = StorageIO::Read(path, 65536);
  root_ = Markup::XamlReader::Load(
              winrt::to_hstring(std::string(bytes.begin(), bytes.end())))
              .as<FrameworkElement>();
  resources_.ApplyLanguage(root_);
  Find<TextBlock>(L"SettingsLogLevelLabel")
      .Text(resources_.GetString(L"SettingsLogLevel"));
  for (auto level : {L"Trace", L"Debug", L"Info", L"Warning", L"Error"})
    Find<ComboBox>(L"SettingsLogLevel")
        .Items()
        .Append(winrt::box_value(winrt::hstring(level)));
  Automation::AutomationProperties::SetName(
      Find<ComboBox>(L"SettingsLogLevel"),
      resources_.GetString(L"SettingsLogLevel"));
  Find<ComboBox>(L"SettingsLogLevel")
      .SelectionChanged([this](auto const&, auto const&) {
        auto index = Find<ComboBox>(L"SettingsLogLevel").SelectedIndex();
        if (!rendering_ && index >= 0 && index <= int(LogLevel::error))
          Input([&] {
            model_.Logging(std::string(
                LogLevelName(static_cast<Win2Air::LogLevel>(index))));
          });
      });
  for (auto key : {L"AboutVersion", L"AboutDisclaimerTitle", L"AboutDisclaimer",
                   L"AboutApple", L"AboutMicrosoft", L"AboutOpenSource",
                   L"AboutOpenSourceTerms", L"ReceiverMediaControl"})
    Find<TextBlock>(key == std::wstring_view(L"ReceiverMediaControl")
                        ? L"ReceiverMediaControlLabel"
                        : key)
        .Text(resources_.GetString(key));
  auto repository = Find<HyperlinkButton>(L"AboutRepository");
  Find<TextBlock>(L"AboutRepositoryAddress").Text(kProjectRepository);
  repository.NavigateUri(winrt::Windows::Foundation::Uri(kProjectRepository));
  Automation::AutomationProperties::SetAutomationId(repository,
                                                    L"AboutRepository");
  Automation::AutomationProperties::SetName(repository, kProjectRepository);
  auto notices = Find<Grid>(L"OpenSourceNotices");
  notices.RowDefinitions().Append(RowDefinition{});
  int column = 0;
  for (auto key :
       {L"AboutProjectLink", L"AboutProjectAddress", L"AboutLicenseLink"}) {
    TextBlock heading;
    heading.Text(resources_.GetString(key));
    heading.Style(Application::Current()
                      .Resources()
                      .Lookup(winrt::box_value(L"AirSecondaryTextStyle"))
                      .as<Style>());
    Grid::SetColumn(heading, column++);
    notices.Children().Append(heading);
  }
  int row = 1;
  for (auto const& notice : kOpenSourceNotices) {
    notices.RowDefinitions().Append(RowDefinition{});
    TextBlock label;
    label.Style(Application::Current()
                    .Resources()
                    .Lookup(winrt::box_value(L"AirSettingLabelStyle"))
                    .as<Style>());
    label.Text(notice.name);
    label.VerticalAlignment(VerticalAlignment::Top);
    Grid::SetRow(label, row);
    notices.Children().Append(label);
    column = 1;
    for (auto const& [text, target, id] :
         {std::tuple{notice.project, notice.project, L"OpenSourceProject_"},
          std::tuple{notice.license, notice.terms, L"OpenSourceLicense_"}}) {
      TextBlock cell;
      cell.FontSize(13);
      cell.TextWrapping(TextWrapping::Wrap);
      Documents::Hyperlink link;
      link.NavigateUri(winrt::Windows::Foundation::Uri(target));
      Documents::Run caption;
      caption.Text(text);
      link.Inlines().Append(caption);
      Automation::AutomationProperties::SetAutomationId(
          link, winrt::hstring(std::wstring(id) + std::wstring(notice.name)));
      Automation::AutomationProperties::SetName(link, winrt::hstring(text));
      cell.Inlines().Append(link);
      Grid::SetRow(cell, row);
      Grid::SetColumn(cell, column++);
      notices.Children().Append(cell);
    }
    ++row;
  }
  resources_.LocalizeSwitch(Find<ToggleSwitch>(L"ReceiverMediaControl"));
  Automation::AutomationProperties::SetName(
      Find<ToggleSwitch>(L"ReceiverMediaControl"),
      resources_.GetString(L"ReceiverMediaControl"));
  Find<ToggleSwitch>(L"ReceiverMediaControl")
      .Toggled([this](auto const&, auto const&) {
        if (!rendering_)
          Input([&] {
            model_.ReceiverMediaControl(
                Find<ToggleSwitch>(L"ReceiverMediaControl").IsOn());
          });
      });
  for (auto name : {L"StartupEnabled", L"StartupMinimized", L"RestorePrevious",
                    L"CloseToTray"})
    Find<TextBlock>((std::wstring(name) + L"Label").c_str())
        .Text(resources_.GetString(name));
  for (auto name : {L"StartupEnabled", L"StartupMinimized", L"RestorePrevious",
                    L"CloseToTray"})
    resources_.LocalizeSwitch(Find<ToggleSwitch>(name));
  for (auto name : {L"SettingsTheme", L"SettingsLanguage", L"SettingsSource"})
    Find<TextBlock>((std::wstring(name) + L"Label").c_str())
        .Text(resources_.GetString(name));
  for (auto key : {L"FollowSystem", L"ThemeLight", L"ThemeDark"})
    Find<ComboBox>(L"SettingsTheme")
        .Items()
        .Append(winrt::box_value(resources_.GetString(key)));
  for (auto key : {L"FollowSystem", L"LanguageChinese", L"LanguageEnglish"})
    Find<ComboBox>(L"SettingsLanguage")
        .Items()
        .Append(winrt::box_value(resources_.GetString(key)));
  Find<TextBlock>(L"SettingsLocalLabel")
      .Text(resources_.GetString(L"LocalPlayback"));
  resources_.LocalizeSwitch(Find<ToggleSwitch>(L"SettingsLocal"));
  for (auto name : {L"SettingsTheme", L"SettingsLanguage", L"SettingsSource",
                    L"SettingsLocal", L"StartupEnabled", L"StartupMinimized",
                    L"RestorePrevious", L"CloseToTray"})
    Automation::AutomationProperties::SetName(
        root_.FindName(name).as<DependencyObject>(),
        resources_.GetString(name == std::wstring_view(L"SettingsLocal")
                                 ? L"LocalPlayback"
                                 : name));
  Find<ToggleSwitch>(L"SettingsLocal")
      .Toggled([this](auto const&, auto const&) {
        if (!rendering_)
          Input([&] {
            model_.Local(Find<ToggleSwitch>(L"SettingsLocal").IsOn());
          });
      });
  for (auto name : {L"StartupMinimized", L"RestorePrevious", L"CloseToTray"})
    Find<ToggleSwitch>(name).Toggled([this](auto const&, auto const&) {
      if (rendering_) return;
      Input([&] {
        model_.Lifecycle(Find<ToggleSwitch>(L"CloseToTray").IsOn(),
                         Find<ToggleSwitch>(L"StartupMinimized").IsOn(),
                         Find<ToggleSwitch>(L"RestorePrevious").IsOn());
      });
    });
  Find<ComboBox>(L"SettingsTheme")
      .SelectionChanged([this](auto const&, auto const&) {
        auto index = Find<ComboBox>(L"SettingsTheme").SelectedIndex();
        const std::string values[] = {"system", "light", "dark"};
        if (!rendering_ && index >= 0 && index < 3) {
          model_.Draft().theme.Edit(values[index]);
          Input([&] {
            model_.Appearance(model_.Draft().theme.Value(),
                              model_.Draft().language.Value());
          });
          Refresh(appearance_busy_);
        }
      });
  Find<ComboBox>(L"SettingsLanguage")
      .SelectionChanged([this](auto const&, auto const&) {
        auto index = Find<ComboBox>(L"SettingsLanguage").SelectedIndex();
        const std::string values[] = {"system", "zh-CN", "en-US"};
        if (!rendering_ && index >= 0 && index < 3) {
          model_.Draft().language.Edit(values[index]);
          Input([&] {
            model_.Appearance(model_.Draft().theme.Value(),
                              model_.Draft().language.Value());
          });
          Refresh(appearance_busy_);
        }
      });
  Find<ComboBox>(L"SettingsSource")
      .SelectionChanged([this](auto const&, auto const&) {
        auto index = Find<ComboBox>(L"SettingsSource").SelectedIndex();
        if (!rendering_ && index >= 0 && size_t(index) < source_ids_.size()) {
          model_.Draft().source.Edit(source_ids_[size_t(index)]);
          Input([&] {
            AudioSourceConfig source;
            source.follow_system_default = index == 0;
            source.endpoint_id = source_ids_[size_t(index)];
            model_.Source(source);
          });
          Refresh(appearance_busy_);
        }
      });
  Refresh(false);
  for (auto pair :
       {std::tuple{L"GeneralCategory", L"\uE771", L"SettingsGeneralCategory"},
        {L"AudioCategory", L"\uE189", L"SettingsAudioCategory"},
        {L"StreamingCategory", L"\uE93E", L"SettingsStreamingCategory"},
        {L"ReceiversCategory", L"\uE772", L"SettingsReceiversCategory"},
        {L"AboutCategory", L"\uE946", L"SettingsAboutCategory"}}) {
    StackPanel category_row;
    category_row.Orientation(Orientation::Horizontal);
    category_row.Spacing(12);
    FontIcon icon;
    icon.Glyph(std::get<1>(pair));
    icon.FontSize(16);
    icon.VerticalAlignment(VerticalAlignment::Center);
    TextBlock label;
    label.Text(resources_.GetString(std::get<2>(pair)));
    label.VerticalAlignment(VerticalAlignment::Center);
    category_row.Children().Append(icon);
    category_row.Children().Append(label);
    auto item = Find<ListViewItem>(std::get<0>(pair));
    item.Content(category_row);
    Automation::AutomationProperties::SetName(item, label.Text());
  }
  Find<ListView>(L"SettingsCategory")
      .SelectionChanged([this](auto const&, auto const&) {
        SelectSection(Find<ListView>(L"SettingsCategory").SelectedIndex());
      });
  SelectSection(0);
}
void SettingsPage::SelectSection(int index) {
  if (index < 0 || index > 4) return;
  auto category = Find<ListView>(L"SettingsCategory");
  if (category.SelectedIndex() != index) category.SelectedIndex(index);
  wchar_t const* panels[] = {L"GeneralPanel", L"AudioPanel", L"StreamingPanel",
                             L"ReceiversPanel", L"AboutPanel"};
  for (int item = 0; item < 5; ++item)
    Find<FrameworkElement>(panels[item])
        .Visibility(item == index ? Visibility::Visible
                                  : Visibility::Collapsed);
}
void SettingsPage::Refresh(bool appearance_busy) {
  model_.Refresh();
  Apply(appearance_busy);
}
void SettingsPage::Present(PlaybackSnapshot snapshot) {
  model_.Present(std::move(snapshot));
  Apply(false);
}
void SettingsPage::Apply(bool appearance_busy) {
  appearance_busy_ = appearance_busy;
  auto const& state = model_.Snapshot();
  auto const& saved = state.preferences;
  rendering_ = true;
  std::vector<std::string> ids{""};
  for (auto const& source : state.sources) ids.push_back(source.id);
  auto const& draft = model_.Draft();
  if (ids != source_ids_) {
    source_ids_ = std::move(ids);
    auto source = Find<ComboBox>(L"SettingsSource");
    source.Items().Clear();
    source.Items().Append(
        winrt::box_value(resources_.GetString(L"FollowDefault")));
    for (size_t index = 0; index < state.sources.size(); ++index) {
      auto const& endpoint = state.sources[index];
      source.Items().Append(winrt::box_value(winrt::to_hstring(endpoint.name)));
    }
  }
  auto match =
      std::find(source_ids_.begin(), source_ids_.end(), draft.source.Value());
  Find<ComboBox>(L"SettingsSource")
      .SelectedIndex(
          match == source_ids_.end() ? -1 : int(match - source_ids_.begin()));
  Find<ComboBox>(L"SettingsTheme")
      .SelectedIndex(draft.theme.Value() == "system"  ? 0
                     : draft.theme.Value() == "light" ? 1
                                                      : 2);
  Find<ComboBox>(L"SettingsLanguage")
      .SelectedIndex(draft.language.Value() == "system"  ? 0
                     : draft.language.Value() == "zh-CN" ? 1
                                                         : 2);
  Find<ToggleSwitch>(L"SettingsLocal").IsOn(saved.local_playback);
  Find<ComboBox>(L"SettingsLogLevel")
      .SelectedIndex(int(ResolveLogLevel(saved.log_level)));
  Find<ComboBox>(L"SettingsLogLevel").IsEnabled(!model_.Pending());
  Find<ToggleSwitch>(L"ReceiverMediaControl")
      .IsOn(saved.receiver_media_control);
  Find<ToggleSwitch>(L"ReceiverMediaControl").IsEnabled(!model_.Pending());
  for (auto pair : {std::pair{L"CloseToTray", saved.close_to_tray},
                    {L"StartupMinimized", saved.minimized},
                    {L"RestorePrevious", saved.restore_previous}})
    Find<ToggleSwitch>(pair.first).IsOn(pair.second);
  for (auto name : {L"SettingsSource", L"SettingsLocal", L"StartupMinimized",
                    L"RestorePrevious", L"CloseToTray"})
    root_.FindName(name).as<Control>().IsEnabled(!model_.Pending());
  for (auto name : {L"SettingsTheme", L"SettingsLanguage"})
    Find<ComboBox>(name).IsEnabled(!model_.Pending() && !appearance_busy);
  std::string alert = input_error_;
  if (alert.empty() && (model_.Result().status == CommandStatus::failed ||
                        model_.Result().status == CommandStatus::rejected))
    alert = model_.Result().message;
  ShowBlockingFailure(root_, alert, resources_.GetString(L"DialogClose"),
                      blocking_);
  rendering_ = false;
}
}  // namespace Win2Air
