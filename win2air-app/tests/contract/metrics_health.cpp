#include "../../src/Metrics/HealthEvaluator.h"
#include "../../src/Metrics/MetricsCollector.h"
#include "../../src/Metrics/MetricsHistory.h"
#include "test_support.h"
using namespace Win2Air;
namespace {
MetricsSnapshot Frame(uint64_t second, double sent, double requests,
                      bool clock = false) {
  MetricsSnapshot frame;
  frame.receiver_id = "AA:BB:CC:DD:EE:FF";
  frame.generation = 1;
  frame.sample_time = second * 1000000000;
  auto number = [&](double value) {
    return Measured(value, "controlled-observation", frame.sample_time);
  };
  frame.sent_packets = number(sent);
  frame.resend_requests = number(requests);
  frame.pending = frame.queue_fill = frame.underruns = number(0);
  if (clock) {
    frame.clock_lock = number(1);
    frame.clock_offset = frame.clock_drift = number(0);
  }
  return frame;
}
}  // namespace
void TestMetricsHealth() {
  MetricsHistoryBuffer buckets;
  for (uint64_t second = 0; second <= 3600; ++second) {
    auto frame = Frame(second, double(second * 100), 0);
    frame.sent_bytes =
        Measured(double(second * 1000), "wire", frame.sample_time);
    frame.target_latency = Measured(90, "target", frame.sample_time);
    frame.target_latency.availability = Availability::estimated;
    buckets.Add(std::move(frame));
  }
  auto bucket_history = buckets.Snapshot();
  for (auto window : {60U, 600U, 3600U}) {
    auto data =
        AggregateHistory(*bucket_history, 3600000000000ULL, window,
                         "AA:BB:CC:DD:EE:FF", HistoryMetric::sent_bytes);
    auto packets =
        AggregateHistory(*bucket_history, 3600000000000ULL, window,
                         "AA:BB:CC:DD:EE:FF", HistoryMetric::sent_packets);
    Require(data.size() == 60 && packets.size() == 60,
            "Each selected range uses bounded time buckets");
    Require(
        data.back().value == double(window / 60 * 1000) &&
            packets.back().value == double(window / 60 * 100),
        "Counter charts show interval totals rather than lifetime counters");
    Require(data.front().value == data.back().value,
            "The oldest complete interval retains its counter baseline");
  }
  MetricsHistory varying_history;
  for (uint64_t second = 0; second <= 3600; ++second) {
    auto frame =
        Frame(second, double(second * second), double(second * second));
    frame.sent_bytes =
        Measured(double(second * second), "wire", frame.sample_time);
    frame.target_latency =
        Measured(double(second), "target", frame.sample_time);
    frame.target_latency.availability = Availability::estimated;
    frame.bitrate = Measured(double(second * 8), "wire", frame.sample_time);
    varying_history.snapshots.push_back(
        std::make_shared<MetricsSnapshot const>(std::move(frame)));
  }
  for (auto window : {600U, 3600U}) {
    auto width = uint64_t(window / 60);
    auto interval_end = 3591 / width * width;
    for (auto metric : {HistoryMetric::sent_bytes, HistoryMetric::sent_packets,
                        HistoryMetric::target_latency, HistoryMetric::bitrate,
                        HistoryMetric::resends}) {
      auto first = AggregateHistory(varying_history, 3591000000000ULL, window,
                                    "AA:BB:CC:DD:EE:FF", metric);
      auto refreshed = AggregateHistory(varying_history, 3599999999999ULL,
                                        window, "AA:BB:CC:DD:EE:FF", metric);
      for (size_t index = 0; index < first.size(); ++index)
        Require(
            first[index].age_seconds == refreshed[index].age_seconds &&
                first[index].value == refreshed[index].value,
            "Long-range history stays fixed until the next interval boundary");
      auto advanced = AggregateHistory(varying_history, 3600000000000ULL,
                                       window, "AA:BB:CC:DD:EE:FF", metric);
      Require(
          advanced[advanced.size() - 2].value == first.back().value,
          "Crossing a fixed boundary shifts completed history by one bucket");
      if (metric == HistoryMetric::sent_bytes)
        Require(first.back().value ==
                    double(interval_end * interval_end -
                           (interval_end - width) * (interval_end - width)),
                "Long-range totals stop at the last complete fixed interval");
    }
  }
  for (uint64_t second = 3601; second <= 3659; ++second)
    buckets.Add(Frame(second, double(second * 100), 0));
  auto retained_hour =
      AggregateHistory(*buckets.Snapshot(), 3659000000000ULL, 3600,
                       "AA:BB:CC:DD:EE:FF", HistoryMetric::sent_packets);
  for (auto const& bucket : retained_hour)
    Require(bucket.value == 6000,
            "Retention preserves every complete interval until the hour view "
            "advances");
  auto delay =
      AggregateHistory(*bucket_history, 3600000000000ULL, 60,
                       "AA:BB:CC:DD:EE:FF", HistoryMetric::target_latency);
  Require(delay.back().value == 90,
          "Presentation delay remains a time-weighted value");
  auto missing_bucket = AggregateHistory(*bucket_history, 3600000000000ULL, 60,
                                         "unknown", HistoryMetric::sent_bytes);
  Require(!missing_bucket.back().value,
          "An unrelated receiver cannot create a zero-valued observation");
  MetricsHistory interrupted;
  auto before_gap = Frame(60, 100, 0);
  auto after_gap = Frame(61, 200, 0);
  after_gap.gap = true;
  interrupted.snapshots = {std::make_shared<MetricsSnapshot const>(before_gap),
                           std::make_shared<MetricsSnapshot const>(after_gap)};
  auto gap_buckets =
      AggregateHistory(interrupted, 61000000000ULL, 60, before_gap.receiver_id,
                       HistoryMetric::sent_packets);
  Require(!gap_buckets.back().value,
          "A disconnected interval is a chart gap rather than interpolated "
          "traffic");
  after_gap.gap = false;
  after_gap.sent_packets.value = 50;
  interrupted.snapshots.back() =
      std::make_shared<MetricsSnapshot const>(after_gap);
  auto reset_buckets =
      AggregateHistory(interrupted, 61000000000ULL, 60, before_gap.receiver_id,
                       HistoryMetric::sent_packets);
  Require(!reset_buckets.back().value,
          "A reset counter cannot produce negative interval traffic");
  MetricsCollector collector;
  sender_snapshot sdk{};
  sdk.size = sizeof(sdk);
  sdk.version = 1;
  sdk.generation = 7;
  sdk.available =
      SENDER_SNAPSHOT_SEND | SENDER_SNAPSHOT_RESEND | SENDER_SNAPSHOT_PENDING;
  sdk.capacity_bytes = 4000;
  sdk.queue_bytes = 1000;
  sdk.sent_packets = 10;
  sdk.sent_bytes = 100;
  sender_timing_info timing{};
  timing.effective_ms = 870;
  collector.BeginGeneration(7, 1000000000, {100, 80, 70, 1, 0});
  auto first = collector.Collect("AA:BB:CC:DD:EE:FF", 7, 2000000000, sdk,
                                 timing, {120, 100, 90, 2, 0}, false, {});
  Require(first.sent_packets.value == 10 && first.queue_fill.value == 0.25 &&
              first.captured_frames.value == 20 && !first.bitrate.value &&
              !first.clock_lock.value && !first.output_latency.value &&
              first.clock_lock.availability == Availability::unavailable &&
              !first.clock_lock.reason.empty(),
          "Metrics use observed counters, generation baselines and explicit "
          "missing measurements");
  sdk.sent_bytes = 300;
  auto second = collector.Collect(first.receiver_id, 7, 3000000000, sdk, timing,
                                  {140, 120, 100, 2, 0}, false, {});
  Require(second.bitrate.value == 1600 && second.captured_frames.value == 40,
          "Bitrate is calculated from wire-byte and monotonic-time deltas");
  auto missing = collector.Collect(first.receiver_id, 0, 4000000000, {}, timing,
                                   {140, 120, 100, 2, 0}, true, {});
  Require(missing.gap && missing.recovering && !missing.sent_packets.value &&
              !missing.captured_frames.value &&
              missing.app_captured_frames == 140,
          "Disconnected generation metrics are unavailable while lifetime "
          "counters remain observed");
  sdk.generation = 8;
  sdk.sent_packets = 0;
  sdk.sent_bytes = 0;
  collector.BeginGeneration(8, 5000000000, {140, 120, 100, 2, 0});
  auto recovered = collector.Collect(first.receiver_id, 8, 6000000000, sdk,
                                     timing, {150, 130, 110, 2, 1}, false, {});
  Require(recovered.recovered_event && recovered.captured_frames.value == 10 &&
              recovered.sent_packets.value == 0,
          "Recovery resets protocol counts without erasing observed "
          "application totals");
  MetricsHistoryBuffer buffer;
  for (uint64_t i = 1; i <= 3665; ++i) buffer.Add(Frame(i, double(i), 0));
  auto history = buffer.Snapshot();
  auto same_samples = buffer.Snapshot();
  Require(
      history->snapshots.front() == same_samples->snapshots.front(),
      "Publishing history shares immutable samples instead of copying metrics");
  auto previous_last = history->snapshots.back();
  MetricsHistory invalid_history;
  invalid_history.snapshots.push_back({});
  Rejects([&] { HistoryWindow(invalid_history, 0, 60, ""); },
          "Missing immutable history samples are rejected");
  Require(history->snapshots.size() == 3661 &&
              history->snapshots.front()->sample_time == 5000000000,
          "History retains a bounded hour with an alignment margin");
  auto gap = Frame(3667, 3667, 0);
  gap.gap = true;
  buffer.Add(gap);
  Require(history->snapshots.size() == 3661 &&
              history->snapshots.back() == previous_last &&
              previous_last->sample_time == 3665000000000,
          "Published history stays unchanged while new samples arrive");
  history = buffer.Snapshot();
  Require(
      !history->gaps.empty() &&
          HistoryWindow(*history, gap.sample_time, 60, gap.receiver_id)
                  .snapshots.size() <= 61,
      "Missed samples are marked and history window selection remains bounded");
  Rejects([&] { HistoryWindow(*history, gap.sample_time, 30, ""); },
          "Unsupported history duration is rejected");
  HealthEvaluator evaluator;
  for (auto [requests, level] :
       {std::pair{20.0, "unstable"}, {5.0, "good"}, {51.0, "poor"}}) {
    evaluator.Reset();
    std::vector samples{Frame(0, 0, 0), Frame(1, 1000, requests)};
    Require(evaluator.Evaluate(samples, 1000000000).level == level,
            "Known resend degradation overrides unavailable receiver clocks");
  }
  evaluator.Reset();
  std::vector samples{Frame(0, 0, 0), Frame(1, 1000, 0)};
  Require(evaluator.Evaluate(samples, 1000000000).level == "unknown",
          "Missing clock evidence does not become excellent from zero resend "
          "counts");
  samples.back().pending = Measured(51, "pending-observation", 1000000000);
  samples.back().sent_packets = {};
  Require(
      evaluator.Evaluate(samples, 1000000000).level == "poor",
      "Observed severe pending work wins even when sent counts are missing");
  evaluator.Reset();
  samples = {Frame(0, 0, 0), Frame(1, 1000, 0)};
  samples.back().pending = Measured(5, "pending-observation", 1000000000);
  auto steady = evaluator.Evaluate(samples, 1000000000);
  Require(steady.level == "good" && steady.reason_codes.empty(),
          "Encoder work within ten packets stays good without an explanation");
  evaluator.Reset();
  samples = {Frame(0, 0, 0), Frame(1, 1000, 0)};
  samples.back().underruns = Measured(1, "playout-observation", 1000000000);
  Require(evaluator.Evaluate(samples, 1000000000).level == "poor",
          "An observed underrun overrides missing clock evidence");
  for (uint64_t i = 2; i <= 7; ++i) {
    auto value = Frame(i, double(i * 1000), 0);
    value.underruns = Measured(1, "playout-observation", value.sample_time);
    samples.push_back(value);
  }
  Require(
      evaluator.Evaluate(samples, 7000000000).level == "unknown",
      "Expired underrun evidence cannot keep cumulative counts poor forever");
  evaluator.Reset();
  samples = {Frame(1, 0, 0)};
  samples.back().recovering = true;
  Require(evaluator.Evaluate(samples, 1000000000).level == "poor",
          "Actual recovery is severe even without packet measurements");
  samples = {Frame(7, 6000, 0, true), Frame(8, 7000, 0, true)};
  auto improving = evaluator.Evaluate(samples, 8000000000);
  Require(improving.level == "poor" &&
              improving.reason_codes == std::vector<std::string>{"recovering"},
          "A known improvement waits for continuous stability and keeps the "
          "current reason");
  samples = {Frame(22, 21000, 0, true), Frame(23, 22000, 0, true)};
  Require(
      evaluator.Evaluate(samples, 23000000000).level == "excellent",
      "Complete evidence becomes excellent only after the stability interval");
  evaluator.Reset();
  samples = {Frame(0, 0, 0), Frame(1, 1000, 0)};
  samples.back().pending = {
      100, Availability::estimated, {}, "estimated", 1000000000};
  Require(evaluator.Evaluate(samples, 1000000000).level == "unknown",
          "Estimated values cannot manufacture observed network degradation");
}
