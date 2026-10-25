#include "StatisticsPage.h"
#include <shobjidl.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>
#include "../Settings/StorageIO.h"
#include "../Streaming/FailureDetail.h"
#include "MetricFormat.h"
#include "ViewModels/ReceiverViewModel.h"
namespace Win2Air {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
namespace Media = winrt::Microsoft::UI::Xaml::Media;
StatisticsPage::StatisticsPage(StreamCoordinator& service,
                               std::filesystem::path const& path, HWND owner)
    : model_(service), owner_(owner) {
  auto bytes = StorageIO::Read(path, 65536);
  root_ = Markup::XamlReader::Load(
              winrt::to_hstring(std::string(bytes.begin(), bytes.end())))
              .as<FrameworkElement>();
  resources_.ApplyLanguage(root_);
  root_.FindName(L"MetricDetailsHeading")
      .as<TextBlock>()
      .Text(resources_.GetString(L"MetricDetails"));
  for (auto name : {L"ExportDiagnostics", L"CancelDiagnostics"})
    root_.FindName(name).as<Button>().Content(
        winrt::box_value(resources_.GetString(name)));
  chart_ = winrt::Win2Air::Charts::TelemetryChart();
  audio_chart_ = winrt::Win2Air::Charts::TelemetryChart();
  auto preferences = model_.ChartPreferences();
  window_seconds_ = preferences.primary.seconds;
  audio_window_seconds_ = preferences.secondary.seconds;
  metric_ = static_cast<HistoryMetric>(preferences.primary.metric);
  audio_metric_ = static_cast<HistoryMetric>(preferences.secondary.metric);
  for (bool audio : {false, true}) {
    std::wstring prefix = audio ? L"AudioHistory" : L"History";
    auto find = [&](wchar_t const* suffix) {
      return root_.FindName(winrt::hstring(prefix) + suffix);
    };
    find(L"Chart").as<ContentControl>().Content(audio ? audio_chart_ : chart_);
    auto window = find(L"Window").as<ComboBox>();
    Automation::AutomationProperties::SetName(
        window, resources_.GetString(L"HistoryWindow"));
    for (auto key : {L"HistoryMinute", L"HistoryTenMinutes", L"HistoryHour"})
      window.Items().Append(winrt::box_value(resources_.GetString(key)));
    auto seconds = audio ? audio_window_seconds_ : window_seconds_;
    window.SelectedIndex(seconds == 60 ? 0 : seconds == 600 ? 1 : 2);
    window.SelectionChanged([this, audio, prefix](auto const&, auto const&) {
      if (rendering_) return;
      auto index = root_.FindName(winrt::hstring(prefix) + L"Window")
                       .as<ComboBox>()
                       .SelectedIndex();
      if (index < 0 || index > 2) return;
      const unsigned seconds[] = {60, 600, 3600};
      (audio ? audio_window_seconds_ : window_seconds_) = seconds[index];
      model_.Window(window_seconds_);
      model_.Chart(audio, unsigned(audio ? audio_metric_ : metric_),
                   audio ? audio_window_seconds_ : window_seconds_);
      Render();
    });
    auto metric = find(L"Metric").as<ComboBox>();
    Automation::AutomationProperties::SetName(
        metric, resources_.GetString(L"HistoryMetric"));
    for (auto key : {L"ChartSentBytes", L"ChartSentPackets", L"LeadLabel",
                     L"BitrateLabel", L"ResendHeading"})
      metric.Items().Append(winrt::box_value(resources_.GetString(key)));
    metric.SelectedIndex(int(audio ? audio_metric_ : metric_));
    metric.SelectionChanged([this, audio, prefix](auto const&, auto const&) {
      if (rendering_) return;
      auto index = root_.FindName(winrt::hstring(prefix) + L"Metric")
                       .as<ComboBox>()
                       .SelectedIndex();
      if (index < 0 || index > 4) return;
      (audio ? audio_metric_ : metric_) = static_cast<HistoryMetric>(index);
      model_.Chart(audio, unsigned(index),
                   audio ? audio_window_seconds_ : window_seconds_);
      Render();
    });
  }
  root_.FindName(L"ExportDiagnostics")
      .as<Button>()
      .Click([this](auto const&, auto const&) {
        try {
          PickExport(owner_, model_.PrepareExport());
        } catch (...) {
          root_.FindName(L"ExportStatus")
              .as<TextBlock>()
              .Text(resources_.GetString(L"DiagnosticFailed"));
          ShowBlockingFailure(root_, DescribeFailure().message,
                              resources_.GetString(L"DialogClose"),
                              export_prompt_);
        }
      });
  root_.FindName(L"CancelDiagnostics")
      .as<Button>()
      .Click([this](auto const&, auto const&) { model_.CancelExport(); });
  for (auto name : {L"DurationLabel", L"LeadLabel", L"BitrateLabel",
                    L"HealthLabel", L"NetworkHeading", L"ConnectionEvents",
                    L"NetworkBytesLabel", L"NetworkPacketsLabel",
                    L"NetworkRequestsLabel", L"NetworkPendingLabel"})
    root_.FindName(name).as<TextBlock>().Text(resources_.GetString(name));
  Refresh();
  theme_changed_ = root_.ActualThemeChanged(
      winrt::auto_revoke, [this](auto const&, auto const&) { Render(); });
  size_changed_ = root_.SizeChanged(
      winrt::auto_revoke, [this](auto const&, auto const&) { Reflow(); });
  Reflow();
}
void StatisticsPage::Reflow() {
  // Runtime-loaded XAML has no Page visual-state host. Use the measured content
  // width so navigation width and system scaling are included in the decision.
  auto scroll = root_.FindName(L"StatisticsScroll").as<ScrollViewer>();
  auto width = scroll.ViewportWidth();
  auto content = root_.FindName(L"StatisticsContent").as<FrameworkElement>();
  if (width > 1 &&
      !(content.Width() > 0 && std::abs(content.Width() - width) <= 1))
    content.Width(width);
  bool compact = root_.ActualWidth() < 800;
  if (compact_ == compact) return;
  compact_ = compact;
  auto place = [this](wchar_t const* name, int row, int column, int span) {
    auto card = root_.FindName(name).as<FrameworkElement>();
    Grid::SetRow(card, row);
    Grid::SetColumn(card, column);
    Grid::SetColumnSpan(card, span);
  };
  place(L"DurationCard", 0, 0, compact ? 2 : 1);
  place(L"LeadCard", 0, compact ? 2 : 1, compact ? 2 : 1);
  place(L"BitrateCard", compact ? 1 : 0, compact ? 0 : 2, compact ? 2 : 1);
  place(L"HealthCard", compact ? 1 : 0, compact ? 2 : 3, compact ? 2 : 1);
  root_.FindName(L"BodyPrimary")
      .as<ColumnDefinition>()
      .Width(GridLength{compact ? 3.0 : 7.0, GridUnitType::Star});
  root_.FindName(L"BodySecondary")
      .as<ColumnDefinition>()
      .Width(GridLength{compact ? 2.0 : 3.0, GridUnitType::Star});
}
winrt::fire_and_forget StatisticsPage::PickExport(HWND owner,
                                                  ExportRequest request) {
  auto operation = request.operation;
  try {
    winrt::Windows::Storage::Pickers::FileSavePicker picker;
    winrt::check_hresult(picker.as<IInitializeWithWindow>()->Initialize(owner));
    picker.SuggestedFileName(L"Win2Air-diagnostics");
    picker.FileTypeChoices().Insert(
        L"ZIP", winrt::single_threaded_vector<winrt::hstring>({L".zip"}));
    auto file = co_await picker.PickSaveFileAsync();
    if (!operation->alive) co_return;
    if (!file) {
      operation->choosing = false;
      operation->result.status = CommandStatus::canceled;
      co_return;
    }
    // The picker creates its selected file; remove only that new empty
    // placeholder.
    auto path = std::filesystem::path(file.Path().c_str());
    if (std::filesystem::file_size(path) != 0)
      throw std::runtime_error(
          "Choose a new archive path to retain existing diagnostics");
    std::filesystem::remove(path);
    StatisticsViewModel::StartExport(std::move(request), path);
  } catch (...) {
    operation->choosing = false;
    operation->result.status = CommandStatus::failed;
    operation->result.message = DescribeFailure().message;
  }
}
void StatisticsPage::Refresh(bool visible) {
  model_.Refresh();
  Show(visible);
}
void StatisticsPage::Present(PlaybackSnapshot snapshot) {
  model_.Present(std::move(snapshot));
  sample_ = 0;
  Show(true);
}
void StatisticsPage::Show(bool visible) {
  // Export completion is collected in the background; controls are drawn when
  // the page is shown.
  if (!visible) return;
  auto preferences = model_.ChartPreferences();
  bool preferences_changed =
      preferences.primary.metric != unsigned(metric_) ||
      preferences.primary.seconds != window_seconds_ ||
      preferences.secondary.metric != unsigned(audio_metric_) ||
      preferences.secondary.seconds != audio_window_seconds_;
  rendering_ = true;
  for (bool audio : {false, true}) {
    auto chart = audio ? preferences.secondary : preferences.primary;
    auto prefix = audio ? L"AudioHistory" : L"History";
    root_.FindName(winrt::hstring(prefix) + L"Metric")
        .as<ComboBox>()
        .SelectedIndex(int(chart.metric));
    root_.FindName(winrt::hstring(prefix) + L"Window")
        .as<ComboBox>()
        .SelectedIndex(chart.seconds == 60    ? 0
                       : chart.seconds == 600 ? 1
                                              : 2);
  }
  window_seconds_ = preferences.primary.seconds;
  audio_window_seconds_ = preferences.secondary.seconds;
  metric_ = static_cast<HistoryMetric>(preferences.primary.metric);
  audio_metric_ = static_cast<HistoryMetric>(preferences.secondary.metric);
  rendering_ = false;
  ShowBlockingFailure(root_, model_.PreferenceError(),
                      resources_.GetString(L"DialogClose"),
                      preferences_prompt_);
  auto const& operation = model_.ExportState();
  root_.FindName(L"ExportDiagnostics")
      .as<Button>()
      .IsEnabled(!operation.running && !operation.choosing);
  root_.FindName(L"CancelDiagnostics")
      .as<Button>()
      .IsEnabled(operation.running || operation.choosing);
  root_.FindName(L"CancelDiagnostics")
      .as<FrameworkElement>()
      .Visibility(operation.running || operation.choosing
                      ? Visibility::Visible
                      : Visibility::Collapsed);
  winrt::hstring export_text;
  if (operation.running || operation.choosing)
    export_text = resources_.GetString(L"DiagnosticPending");
  else if (operation.result.status == CommandStatus::applied)
    export_text = resources_.GetString(L"DiagnosticSaved");
  else if (operation.result.status == CommandStatus::failed ||
           operation.result.status == CommandStatus::rejected)
    export_text = resources_.GetString(L"DiagnosticFailed");
  auto export_status = root_.FindName(L"ExportStatus").as<TextBlock>();
  export_status.Text(export_text);
  export_status.Visibility(export_text.empty() ? Visibility::Collapsed
                                               : Visibility::Visible);
  ShowBlockingFailure(root_,
                      operation.result.status == CommandStatus::failed ||
                              operation.result.status == CommandStatus::rejected
                          ? operation.result.message
                          : std::string{},
                      resources_.GetString(L"DialogClose"), export_prompt_);
  if (preferences_changed || sample_ != model_.Snapshot().metrics.sample_time) {
    sample_ = model_.Snapshot().metrics.sample_time;
    Render();
  }
}
}  // namespace Win2Air
