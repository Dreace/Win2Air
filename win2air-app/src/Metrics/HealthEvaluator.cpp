#include "HealthEvaluator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Win2Air {
namespace {
bool Known(MetricValue const& value) {
  return value.availability == Availability::measured && value.value &&
         std::isfinite(*value.value) && !value.source.empty();
}
int Rank(std::string const& level) {
  return level == "poor"       ? 3
         : level == "unstable" ? 2
         : level == "good"     ? 1
                               : 0;
}
}  // namespace
void HealthEvaluator::Reset() {
  current_ = {};
  candidate_.clear();
  candidate_since_ = 0;
}
HealthSnapshot HealthEvaluator::Evaluate(
    std::span<MetricsSnapshot const> history, uint64_t now) {
  std::vector<MetricsSnapshot const*> samples;
  samples.reserve(history.size());
  for (auto const& sample : history) samples.push_back(&sample);
  return EvaluateSamples(samples, now);
}
HealthSnapshot HealthEvaluator::Evaluate(
    std::span<ImmutableMetricsSnapshot const> history, uint64_t now) {
  std::vector<MetricsSnapshot const*> samples;
  samples.reserve(history.size());
  for (auto const& sample : history) {
    if (!sample)
      throw std::invalid_argument("Health history sample is missing");
    samples.push_back(sample.get());
  }
  return EvaluateSamples(samples, now);
}
HealthSnapshot HealthEvaluator::EvaluateSamples(
    std::span<MetricsSnapshot const* const> history, uint64_t now) {
  HealthSnapshot next;
  bool poor = false, unstable = false, good = false, complete = false;
  std::vector<MetricsSnapshot const*> window;
  uint64_t begin = now > 5000000000 ? now - 5000000000 : 0;
  auto identity = history.empty() ? "" : history.back()->receiver_id;
  if (identity != receiver_) {
    Reset();
    receiver_ = identity;
  }
  auto add = [&](std::string reason) {
    next.reason_codes.push_back(std::move(reason));
  };
  MetricsSnapshot const* previous = nullptr;
  for (auto pointer : history) {
    auto const& sample = *pointer;
    if (sample.sample_time > now || sample.receiver_id != identity) continue;
    if (sample.sample_time < begin) {
      previous = &sample;
      continue;
    }
    window.push_back(&sample);
    if (sample.recovering) {
      poor = true;
      add("recovering");
    }
    if (sample.recovered_event) {
      unstable = true;
      add("recovered-in-window");
    }
    if (Known(sample.pending)) {
      next.inputs_available.push_back("pending");
      if (*sample.pending.value > 50) {
        poor = true;
        add("pending-above-50");
      } else if (*sample.pending.value > 10) {
        unstable = true;
        add("pending-above-10");
      } else if (*sample.pending.value > 0) {
        good = true;
      }
    }
    if (Known(sample.queue_fill) && *sample.queue_fill.value > 0.9) {
      unstable = true;
      add("queue-headroom-low");
    }
    if (Known(sample.clock_lock) && *sample.clock_lock.value == 0) {
      good = true;
      add("clock-not-locked");
    }
    if (Known(sample.underruns) && *sample.underruns.value > 0 &&
        (!previous || sample.generation != previous->generation ||
         !Known(previous->underruns) ||
         *sample.underruns.value > *previous->underruns.value)) {
      poor = true;
      add("playout-underrun");
    }
    previous = &sample;
  }
  if (window.size() >= 2) {
    auto const& first = *window.front();
    auto const& last = *window.back();
    if (first.generation == last.generation && Known(first.sent_packets) &&
        Known(last.sent_packets) && Known(first.resend_requests) &&
        Known(last.resend_requests) &&
        *last.sent_packets.value > *first.sent_packets.value &&
        *last.resend_requests.value >= *first.resend_requests.value) {
      double ratio =
          (*last.resend_requests.value - *first.resend_requests.value) /
          (*last.sent_packets.value - *first.sent_packets.value);
      next.inputs_available.push_back("resend-rate");
      if (ratio > 0.05) {
        poor = true;
        add("resend-above-5-percent");
      } else if (ratio > 0.01) {
        unstable = true;
        add("resend-above-1-percent");
      } else if (ratio > 0.001) {
        good = true;
        add("resend-above-point-one-percent");
      }
      complete = std::none_of(window.begin(), window.end(),
                              [](auto sample) { return sample->gap; }) &&
                 Known(last.pending) && Known(last.queue_fill) &&
                 Known(last.underruns) && Known(last.clock_lock) &&
                 Known(last.clock_offset) && Known(last.clock_drift);
    }
  }
  next.level = poor       ? "poor"
               : unstable ? "unstable"
               : good     ? "good"
               : complete ? "excellent"
                          : "unknown";
  if (next.level == "unknown") add("missing-window-or-clock-observation");
  auto unique = [](auto& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  };
  unique(next.reason_codes);
  unique(next.inputs_available);
  if (next.level != "unknown" && next.level != current_.level &&
      (Rank(next.level) < Rank(current_.level) || next.level == "excellent")) {
    if (candidate_ != next.level) {
      candidate_ = next.level;
      candidate_since_ = now;
    }
    if (now - candidate_since_ < 15000000000) {
      auto held = current_;
      held.inputs_available = next.inputs_available;
      return held;
    }
  } else {
    candidate_.clear();
    candidate_since_ = 0;
  }
  current_ = std::move(next);
  return current_;
}
}  // namespace Win2Air
