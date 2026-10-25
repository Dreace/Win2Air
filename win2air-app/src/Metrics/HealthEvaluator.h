#pragma once
#include <span>
#include "../Streaming/SessionModels.h"
namespace Win2Air {
class HealthEvaluator {
  HealthSnapshot current_;
  std::string candidate_;
  uint64_t candidate_since_{};
  std::string receiver_;
  HealthSnapshot EvaluateSamples(std::span<MetricsSnapshot const* const>,
                                 uint64_t now);

 public:
  HealthSnapshot Evaluate(std::span<MetricsSnapshot const>, uint64_t now);
  HealthSnapshot Evaluate(std::span<ImmutableMetricsSnapshot const>,
                          uint64_t now);
  void Reset();
};
}  // namespace Win2Air
