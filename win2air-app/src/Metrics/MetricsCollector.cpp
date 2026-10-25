#include "MetricsCollector.h"
#include <cmath>
#include <stdexcept>
namespace Win2Air {
MetricValue Measured(double value, std::string source, uint64_t time) {
  if (!std::isfinite(value) || source.empty())
    throw std::invalid_argument(
        "A measured metric requires a finite value and source");
  return {value, Availability::measured, {}, std::move(source), time};
}
MetricValue Missing(std::string reason, std::string source, uint64_t time) {
  return {std::nullopt, Availability::unavailable, std::move(reason),
          std::move(source), time};
}
void MetricsCollector::BeginGeneration(uint64_t generation, uint64_t now,
                                       CaptureCounters counts) {
  generation_ = generation;
  started_ = now;
  baseline_ = counts;
  previous_bytes_ = 0;
  previous_time_ = 0;
}
MetricsSnapshot MetricsCollector::Collect(
    std::string identity, uint64_t generation, uint64_t now,
    std::optional<sender_snapshot> const& protocol,
    sender_timing_info const& timing, CaptureCounters counts, bool recovering,
    std::optional<CommandResult> error) {
  if (previous_time_ && now < previous_time_)
    throw std::invalid_argument("Metrics time must be monotonic");
  MetricsSnapshot result;
  result.receiver_id = std::move(identity);
  result.generation = generation;
  result.sample_time = now;
  result.format = {44100, 2, 16, 3};
  result.recovering = recovering;
  result.recovered_event =
      generation && counts.recoveries > previous_recoveries_;
  bool same = generation && generation == generation_;
  if (!same) {
    baseline_ = counts;
    started_ = now;
    previous_bytes_ = 0;
    previous_time_ = 0;
  }
  result.uptime = generation ? now - started_ : 0;
  result.last_error = std::move(error);
  result.gap = !protocol;
  auto absent = Missing("no-active-protocol", "sender.snapshot", now);
  result.sent_packets = result.sent_bytes = result.resend_requests =
      result.resend_success = result.resend_expired = result.pending =
          result.queue_fill = result.bitrate = absent;
  if (protocol) {
    auto const& p = *protocol;
    if (p.size != sizeof(p) || p.version != 1 || p.generation != generation)
      throw std::invalid_argument(
          "Protocol metrics belong to another generation");
    auto observed = [&](uint64_t value, std::string source) {
      return Measured(static_cast<double>(value), std::move(source), now);
    };
    if (p.available & SENDER_SNAPSHOT_SEND) {
      result.sent_packets =
          observed(p.sent_packets, "sender.rtp.original-packets");
      result.sent_bytes = observed(p.sent_bytes, "sender.rtp.wire-bytes");
      if (same && previous_time_ && now > previous_time_ &&
          p.sent_bytes >= previous_bytes_)
        result.bitrate = Measured(double(p.sent_bytes - previous_bytes_) * 8e9 /
                                      double(now - previous_time_),
                                  "sender.rtp.wire-byte-delta", now);
    }
    if (p.available & SENDER_SNAPSHOT_RESEND) {
      result.resend_requests =
          observed(p.resend_requests, "sender.rtp.requested-packets");
      result.resend_success =
          observed(p.resend_success, "sender.rtp.successful-resends");
      result.resend_expired =
          observed(p.resend_expired, "sender.rtp.expired-requests");
    }
    if (p.available & SENDER_SNAPSHOT_PENDING)
      result.pending = Measured(std::ceil(double(p.pending_samples) / 352),
                                "sender.encoder.pending-packets", now);
    if (p.capacity_bytes && p.queue_bytes <= p.capacity_bytes)
      result.queue_fill =
          Measured(double(p.queue_bytes) / double(p.capacity_bytes),
                   "sender.pcm.queue-fill", now);
    previous_bytes_ = p.sent_bytes;
  }
  result.underruns = Missing("playout-unobserved", "sender.playout", now);
  result.clock_lock = result.clock_offset = result.clock_drift =
      Missing("clock-unobserved", "sender.ptp", now);
  result.output_latency = Missing("acoustic-unobserved", "acoustic", now);
  result.target_latency =
      Measured(timing.effective_ms, "app.presentation.target-ms", now);
  result.captured_frames = Measured(
      double(counts.captured - baseline_.captured), "app.capture.frames", now);
  result.submitted_frames = Measured(
      double(counts.submitted - baseline_.submitted), "app.submit.frames", now);
  result.nonzero_frames = Measured(double(counts.nonzero - baseline_.nonzero),
                                   "app.capture.nonzero-frames", now);
  result.capture_discontinuities =
      Measured(double(counts.discontinuities - baseline_.discontinuities),
               "app.capture.discontinuities", now);
  result.reconnect_count =
      Measured(double(counts.recoveries), "app.lifetime.reconnections", now);
  result.app_captured_frames = counts.captured;
  result.app_submitted_frames = counts.submitted;
  if (!generation) {
    result.captured_frames = result.submitted_frames = result.nonzero_frames =
        result.capture_discontinuities =
            Missing("no-active-source", "app.capture", now);
  }
  previous_time_ = now;
  previous_recoveries_ = counts.recoveries;
  generation_ = generation;
  return result;
}
}  // namespace Win2Air
