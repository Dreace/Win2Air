#include "MetricsHistory.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
namespace Win2Air {
void MetricsHistoryBuffer::Add(MetricsSnapshot value) {
  if (!samples_.empty() && value.sample_time <= samples_.back()->sample_time)
    throw std::invalid_argument("History samples must increase monotonically");
  if (value.gap ||
      (!samples_.empty() &&
       (samples_.back()->generation != value.generation ||
        value.sample_time - samples_.back()->sample_time > 1500000000)))
    gaps_.push_back(value.sample_time);
  // Publishing history shares immutable samples instead of copying their
  // strings on the audio thread.
  samples_.push_back(std::make_shared<MetricsSnapshot const>(std::move(value)));
  auto now = samples_.back()->sample_time;
  // Keep an extra minute and its counter baseline for the aligned hour view.
  while (samples_.size() > 3661 ||
         now - samples_.front()->sample_time > 3660000000000)
    samples_.pop_front();
  while (!gaps_.empty() &&
         (gaps_.size() > 3660 || gaps_.front() < samples_.front()->sample_time))
    gaps_.pop_front();
}
std::shared_ptr<MetricsHistory const> MetricsHistoryBuffer::Snapshot() const {
  auto value = std::make_shared<MetricsHistory>();
  value->generation = samples_.empty() ? 0 : samples_.back()->generation;
  value->snapshots.assign(samples_.begin(), samples_.end());
  value->gaps.assign(gaps_.begin(), gaps_.end());
  return value;
}
MetricsHistory HistoryWindow(MetricsHistory const& history, uint64_t now,
                             unsigned seconds, std::string const& receiver) {
  if (seconds != 60 && seconds != 600 && seconds != 3600)
    throw std::invalid_argument("Unsupported statistics history window");
  uint64_t span = uint64_t(seconds) * 1000000000;
  auto begin = now > span ? now - span : 0;
  MetricsHistory result;
  result.generation = history.generation;
  for (auto const& sample : history.snapshots) {
    if (!sample) throw std::invalid_argument("History sample is missing");
    if (sample->sample_time >= begin && sample->sample_time <= now &&
        (receiver.empty() || sample->receiver_id == receiver))
      result.snapshots.push_back(sample);
  }
  for (auto gap : history.gaps)
    if (gap >= begin && gap <= now) result.gaps.push_back(gap);
  return result;
}
std::vector<HistoryBucket> AggregateHistory(MetricsHistory const& history,
                                            uint64_t now, unsigned seconds,
                                            std::string const& receiver,
                                            HistoryMetric metric) {
  if (seconds != 60 && seconds != 600 && seconds != 3600)
    throw std::invalid_argument("Unsupported statistics history window");
  if (metric < HistoryMetric::sent_bytes || metric > HistoryMetric::resends)
    throw std::invalid_argument("Unsupported statistics history metric");
  constexpr size_t count = 60;
  uint64_t width = uint64_t(seconds / count) * 1000000000ULL;
  // Long ranges use completed fixed intervals, so refreshing within an interval
  // leaves historical values and chart positions unchanged.
  auto aligned_now = seconds >= 600 ? now / width * width : now;
  std::array<uint64_t, count> begin{}, end{}, covered{};
  std::array<double, count> total{};
  std::vector<HistoryBucket> result(count);
  for (size_t index = 0; index < count; ++index) {
    auto age = (count - index - 1) * width;
    end[index] = aligned_now >= age ? aligned_now - age : 0;
    begin[index] = end[index] >= width ? end[index] - width : 0;
    result[index].age_seconds = -double(age + width / 2) / 1000000000.0;
  }
  auto value = [metric](MetricsSnapshot const& sample) -> MetricValue const& {
    switch (metric) {
      case HistoryMetric::sent_bytes:
        return sample.sent_bytes;
      case HistoryMetric::sent_packets:
        return sample.sent_packets;
      case HistoryMetric::target_latency:
        return sample.target_latency;
      case HistoryMetric::bitrate:
        return sample.bitrate;
      case HistoryMetric::resends:
        return sample.resend_requests;
    }
    throw std::invalid_argument("Unsupported statistics history metric");
  };
  auto known = [metric](MetricValue const& sample) {
    return (sample.availability == Availability::measured ||
            (metric == HistoryMetric::target_latency &&
             sample.availability == Availability::estimated)) &&
           sample.value && std::isfinite(*sample.value) && *sample.value >= 0;
  };
  bool counter = metric == HistoryMetric::sent_bytes ||
                 metric == HistoryMetric::sent_packets ||
                 metric == HistoryMetric::resends;
  for (size_t index = 1; index < history.snapshots.size(); ++index) {
    if (!history.snapshots[index - 1] || !history.snapshots[index])
      throw std::invalid_argument("History sample is missing");
    auto const& before = *history.snapshots[index - 1];
    auto const& after = *history.snapshots[index];
    if (after.sample_time <= before.sample_time)
      throw std::invalid_argument(
          "History samples must increase monotonically");
    auto duration = after.sample_time - before.sample_time;
    if (before.gap || after.gap || duration > 1500000000ULL ||
        before.generation != after.generation ||
        before.receiver_id != after.receiver_id ||
        (!receiver.empty() && after.receiver_id != receiver) ||
        !known(value(before)) || !known(value(after)))
      continue;
    auto first = *value(before).value, last = *value(after).value;
    if (counter && last < first) continue;
    // Counter increments are distributed across the time they cover, rather
    // than plotted as cumulative totals or fabricated zeroes through gaps.
    auto rate = counter ? (last - first) / double(duration) : first;
    for (size_t bucket = 0; bucket < count; ++bucket) {
      auto left = std::max(begin[bucket], before.sample_time);
      auto right = std::min(end[bucket], after.sample_time);
      if (right <= left) continue;
      covered[bucket] += right - left;
      total[bucket] += rate * double(right - left);
    }
  }
  for (size_t index = 0; index < count; ++index) {
    if (end[index] - begin[index] != width || covered[index] != width) continue;
    result[index].value = metric == HistoryMetric::resends
                              ? total[index] * 1000000000.0 / double(width)
                          : counter ? total[index]
                                    : total[index] / double(width);
  }
  return result;
}
}  // namespace Win2Air
