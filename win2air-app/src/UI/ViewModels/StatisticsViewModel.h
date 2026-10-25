#pragma once
#include "../../Diagnostics/DiagnosticsExporter.h"
#include "../../Metrics/MetricsHistory.h"
#include "OperationFeedback.h"
namespace Win2Air {
std::optional<double> ResendRequestsPerSecond(MetricsSnapshot const& previous,
                                              MetricsSnapshot const& current);
struct ExportOperation {
  std::atomic<bool> canceled{}, alive{true};
  std::future<CommandResult> completion;
  bool choosing{}, running{};
  CommandResult result;
};
struct ExportRequest {
  PlaybackSnapshot state;
  std::filesystem::path logs;
  std::shared_ptr<ExportOperation> operation;
  std::function<void()> flush_logs;
};
class StatisticsViewModel {
  StreamCoordinator& service_;
  PlaybackSnapshot state_;
  MetricsHistory history_;
  unsigned window_{60};
  AppSettings::StatisticsPreference preferences_;
  OperationFeedback preference_feedback_;
  uint64_t sampled_{};
  std::shared_ptr<ExportOperation> export_{std::make_shared<ExportOperation>()};

 public:
  explicit StatisticsViewModel(StreamCoordinator& service) : service_(service) {
    preferences_ = service.Snapshot().preferences.statistics;
    window_ = preferences_.primary.seconds;
  }
  ~StatisticsViewModel();
  void Refresh();
  void Present(PlaybackSnapshot snapshot);
  void Window(unsigned seconds);
  void Chart(bool audio, unsigned metric, unsigned seconds);
  AppSettings::StatisticsPreference const& ChartPreferences() const {
    return preferences_;
  }
  std::string PreferenceError() const { return preference_feedback_.Error(); }
  PlaybackSnapshot const& Snapshot() const { return state_; }
  MetricsHistory const& History() const { return history_; }
  ExportOperation const& ExportState() const { return *export_; }
  ExportRequest PrepareExport();
  static void StartExport(ExportRequest, std::filesystem::path);
  void CancelExport();
};
}  // namespace Win2Air
