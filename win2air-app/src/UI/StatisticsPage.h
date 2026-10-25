#pragma once
#include <windows.h>
#include <optional>
#undef GetCurrentTime
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Win2Air.Charts.h>
#include <string>
#include <vector>
#include "../Metrics/MetricsHistory.h"
#include "LocalizedResources.h"
#include "ViewModels/StatisticsViewModel.h"
namespace Win2Air {
class StatisticsPage {
  struct ConnectionEvent {
    std::wstring clock;
    std::wstring text;
  };
  StatisticsViewModel model_;
  HWND owner_;
  winrt::Microsoft::UI::Xaml::FrameworkElement root_{nullptr};
  LocalizedResources resources_;
  bool rendering_{};
  unsigned window_seconds_{60};
  HistoryMetric metric_{HistoryMetric::bitrate};
  winrt::Win2Air::Charts::TelemetryChart chart_{nullptr};
  unsigned audio_window_seconds_{60};
  HistoryMetric audio_metric_{HistoryMetric::target_latency};
  winrt::Win2Air::Charts::TelemetryChart audio_chart_{nullptr};
  uint64_t sample_{};
  std::vector<ConnectionEvent> connection_events_;
  std::vector<std::string> seen_receivers_;
  bool seeded_receivers_{};
  std::string noted_session_id_;
  std::wstring noted_subject_;
  std::wstring noted_action_;
  SessionState noted_state_{SessionState::disconnected};
  bool noted_session_{};
  uint64_t noted_notice_{};
  std::optional<bool> compact_;
  FailurePrompt metrics_prompt_;
  FailurePrompt preferences_prompt_;
  FailurePrompt export_prompt_;
  winrt::Microsoft::UI::Xaml::FrameworkElement::ActualThemeChanged_revoker
      theme_changed_;
  winrt::Microsoft::UI::Xaml::FrameworkElement::SizeChanged_revoker
      size_changed_;
  static winrt::fire_and_forget PickExport(HWND, ExportRequest);
  void Render();
  void Show(bool visible);
  void Reflow();
  void NoteConnections(PlaybackSnapshot const& state);

 public:
  StatisticsPage(StreamCoordinator&, std::filesystem::path const&, HWND);
  auto Content() const { return root_; }
  void Refresh(bool visible = true);
  void Present(PlaybackSnapshot snapshot);
  void CancelExport() { model_.CancelExport(); }
  bool ExportBusy() const {
    return model_.ExportState().running || model_.ExportState().choosing;
  }
};
}  // namespace Win2Air
