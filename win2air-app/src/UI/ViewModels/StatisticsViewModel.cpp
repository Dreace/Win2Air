#include "StatisticsViewModel.h"
#include <cmath>
#include "../../Streaming/FailureDetail.h"
namespace Win2Air {
std::optional<double> ResendRequestsPerSecond(MetricsSnapshot const& previous,
                                              MetricsSnapshot const& current) {
  if (previous.receiver_id != current.receiver_id ||
      previous.generation != current.generation || previous.gap ||
      current.gap || current.sample_time <= previous.sample_time ||
      current.sample_time - previous.sample_time > 1500000000 ||
      !previous.resend_requests.value || !current.resend_requests.value ||
      previous.resend_requests.availability != Availability::measured ||
      current.resend_requests.availability != Availability::measured)
    return std::nullopt;
  auto before = *previous.resend_requests.value;
  auto after = *current.resend_requests.value;
  if (!std::isfinite(before) || !std::isfinite(after) || before < 0 ||
      after < before)
    return std::nullopt;
  // Counter resets and missing windows are discontinuities, not bursts or
  // zeroes.
  return (after - before) * 1000000000.0 /
         double(current.sample_time - previous.sample_time);
}
StatisticsViewModel::~StatisticsViewModel() {
  export_->alive = false;
  export_->canceled = true;
  if (export_->completion.valid()) export_->completion.wait();
}
void StatisticsViewModel::Present(PlaybackSnapshot snapshot) {
  preferences_ = snapshot.preferences.statistics;
  state_ = std::move(snapshot);
  sampled_ = state_.metrics.sample_time;
  window_ = preferences_.primary.seconds;
  history_ = state_.metrics_history
                 ? HistoryWindow(*state_.metrics_history, sampled_, window_,
                                 state_.metrics.receiver_id)
                 : MetricsHistory{};
}
void StatisticsViewModel::Refresh() {
  preference_feedback_.Refresh();
  if (export_->completion.valid() &&
      export_->completion.wait_for(std::chrono::seconds(0)) ==
          std::future_status::ready) {
    export_->result = export_->completion.get();
    export_->running = false;
  }
  auto next = service_.Snapshot();
  if (!preference_feedback_.Pending())
    preferences_ = next.preferences.statistics;
  auto previous_sample = sampled_;
  state_ = std::move(next);
  if (state_.metrics.sample_time != previous_sample) {
    sampled_ = state_.metrics.sample_time;
    history_ = state_.metrics_history
                   ? HistoryWindow(*state_.metrics_history, sampled_, window_,
                                   state_.metrics.receiver_id)
                   : MetricsHistory{};
  }
}
void StatisticsViewModel::Chart(bool audio, unsigned metric, unsigned seconds) {
  if (metric > 4 || (seconds != 60 && seconds != 600 && seconds != 3600))
    throw std::invalid_argument("Invalid statistics chart preference");
  (audio ? preferences_.secondary : preferences_.primary) = {metric, seconds};
  preference_feedback_.Submit(service_.SetStatistics(preferences_));
}
void StatisticsViewModel::Window(unsigned seconds) {
  if (seconds != 60 && seconds != 600 && seconds != 3600)
    throw std::invalid_argument("Unsupported statistics time window");
  window_ = seconds;
  sampled_ = 0;
  Refresh();
}
ExportRequest StatisticsViewModel::PrepareExport() {
  if (export_->running || export_->choosing)
    throw std::runtime_error("A diagnostic export is already active");
  export_->canceled = false;
  export_->result = {};
  export_->choosing = true;
  return {service_.Snapshot(), service_.DataRoot() / L"logs", export_,
          service_.LogFlusher()};
}
void StatisticsViewModel::StartExport(ExportRequest request,
                                      std::filesystem::path path) {
  auto operation = request.operation;
  operation->choosing = false;
  if (!operation->alive || operation->canceled) {
    operation->result.status = CommandStatus::canceled;
    return;
  }
  operation->running = true;
  std::weak_ptr<ExportOperation> weak = operation;
  request.operation.reset();
  operation->completion = std::async(
      std::launch::async,
      [request = std::move(request), path = std::move(path), weak] {
        CommandResult result;
        bool apartment = false;
        try {
          winrt::init_apartment(winrt::apartment_type::multi_threaded);
          apartment = true;
          auto canceled = [&] {
            auto current = weak.lock();
            return !current || !current->alive || current->canceled.load();
          };
          if (canceled())
            throw std::runtime_error("Diagnostic export canceled");
          if (request.flush_logs) request.flush_logs();
          auto files = BuildDiagnostics(
              request.state,
              DiagnosticsExporter::ReadLogs(request.logs, canceled), canceled);
          DiagnosticsExporter::Export(path, files, canceled);
          result.status = CommandStatus::applied;
        } catch (...) {
          auto current = weak.lock();
          result.status = !current || current->canceled
                              ? CommandStatus::canceled
                              : CommandStatus::failed;
          result.message = DescribeFailure().message;
        }
        if (apartment) winrt::uninit_apartment();
        return result;
      });
}
void StatisticsViewModel::CancelExport() { export_->canceled = true; }
}  // namespace Win2Air
