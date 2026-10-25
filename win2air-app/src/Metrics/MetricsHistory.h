#pragma once
#include <deque>
#include "../Streaming/SessionModels.h"
namespace Win2Air {
class MetricsHistoryBuffer {
  std::deque<ImmutableMetricsSnapshot> samples_;
  std::deque<uint64_t> gaps_;

 public:
  void Add(MetricsSnapshot);
  std::shared_ptr<MetricsHistory const> Snapshot() const;
};
MetricsHistory HistoryWindow(MetricsHistory const&, uint64_t now,
                             unsigned seconds, std::string const& receiver);
enum class HistoryMetric {
  sent_bytes,
  sent_packets,
  target_latency,
  bitrate,
  resends
};
struct HistoryBucket {
  double age_seconds{};
  std::optional<double> value;
};
std::vector<HistoryBucket> AggregateHistory(MetricsHistory const&, uint64_t now,
                                            unsigned seconds,
                                            std::string const& receiver,
                                            HistoryMetric metric);
}  // namespace Win2Air
