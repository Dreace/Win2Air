#pragma once
#include "../../../airplay2-sender-core/include/sender.h"
#include "../Streaming/SessionModels.h"
namespace Win2Air {
struct CaptureCounters {
  uint64_t captured{}, submitted{}, nonzero{}, discontinuities{}, recoveries{};
};
MetricValue Measured(double value, std::string source, uint64_t time);
MetricValue Missing(std::string reason, std::string source, uint64_t time);
class MetricsCollector {
  uint64_t generation_{}, started_{}, previous_time_{}, previous_bytes_{};
  CaptureCounters baseline_;
  uint64_t previous_recoveries_{};

 public:
  void BeginGeneration(uint64_t generation, uint64_t now,
                       CaptureCounters counts);
  MetricsSnapshot Collect(std::string identity, uint64_t generation,
                          uint64_t now,
                          std::optional<sender_snapshot> const& protocol,
                          sender_timing_info const&, CaptureCounters,
                          bool recovering, std::optional<CommandResult>);
};
}  // namespace Win2Air
