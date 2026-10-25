#include "StreamCoordinatorInternal.h"

#include <avrt.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <thread>
#include <utility>

#include "../Settings/StorageIO.h"
#include "FailureDetail.h"
#include "LatencyProfile.h"
#include "ReceiverSession.h"
namespace Win2Air {
void SENDER_CALL StreamCoordinator::SafeLog(
    void *context, sender_log_record const *record) noexcept {
  auto &self = *static_cast<StreamCoordinator *>(context);
  if (!record || record->size != sizeof(*record) || record->version != 1 ||
      record->generation != self.generation_ ||
      self.active_intent_ != self.intent_epoch_)
    return;
  try {
    if (record->event > 2 || self.protocol_logs_.size() >= 128)
      throw std::runtime_error("Safe protocol events exceeded their bound");
    self.protocol_logs_.push_back(*record);
  } catch (...) {
    self.log_failed_ = true;
  }
}
void StreamCoordinator::DrainLogs() {
  if (log_failed_) {
    std::lock_guard lock(mutex_);
    snapshot_.diagnostics_error = "Safe protocol events could not be retained";
    log_failed_ = false;
  }
  while (!protocol_logs_.empty()) {
    auto record = protocol_logs_.front();
    protocol_logs_.pop_front();
    try {
      if (audio_services_.log_sink)
        audio_services_.log_sink(
            {record.code ? LogLevel::error : LogLevel::debug,
             LogModule::streaming,
             record.code         ? LogEvent::protocol_failed
             : record.event == 0 ? LogEvent::protocol_state_observed
             : record.event == 1 ? LogEvent::protocol_step_observed
                                 : LogEvent::protocol_control_observed,
             selected_, record.state, record.code, record.platform_code,
             record.peer_code, record.generation});
    } catch (...) {
      std::lock_guard lock(mutex_);
      snapshot_.diagnostics_error = DescribeFailure().message;
    }
  }
}
void StreamCoordinator::SampleMetrics() {
  auto now = audio_services_.metrics_now ? audio_services_.metrics_now()
                                         : MonotonicNs();
  if (last_metrics_ &&
      (now <= last_metrics_ || now - last_metrics_ < 1000000000))
    return;
  auto media = audio_services_.now_playing ? audio_services_.now_playing()
               : system_media_             ? system_media_->Snapshot()
                                           : nullptr;
  std::shared_ptr<NowPlaying const> previous_media;
  {
    std::lock_guard lock(mutex_);
    previous_media = snapshot_.now_playing;
  }
  if (session_.session && media && !media->title.empty() &&
      (!previous_media || media->title != previous_media->title ||
       media->artist != previous_media->artist ||
       media->album != previous_media->album)) {
    auto content = media->title;
    if (!media->artist.empty()) content += " | " + media->artist;
    AddNotice("content", std::move(content));
    Note(LogLevel::info, LogModule::streaming, LogEvent::session_changed,
         "operation=now-playing-changed");
  }
  std::string metadata_error;
  {
    std::lock_guard lock(mutex_);
    snapshot_.now_playing = media;
    snapshot_.metadata_error = media ? media->error : std::string{};
    if (!metadata_transport_error_.empty()) {
      if (!snapshot_.metadata_error.empty()) snapshot_.metadata_error += "; ";
      snapshot_.metadata_error += metadata_transport_error_;
    }
    metadata_error = snapshot_.metadata_error;
  }
  if (metadata_error != logged_metadata_error_) {
    if (!metadata_error.empty())
      Note(LogLevel::warning, LogModule::streaming, LogEvent::command_failed,
           "operation=media-metadata error=" + metadata_error);
    logged_metadata_error_ = metadata_error;
  }
  if (runtime_ && generation_ && session_.session &&
      (session_.session->state == SessionState::ready ||
       session_.session->state == SessionState::streaming)) {
    uint32_t percent{};
    uint64_t revision{};
    sender_error control_error{};
    if (media && (media_generation_ != generation_ ||
                  media_revision_ != media->revision)) {
      auto result = library_.NowPlayingMetadata(
          runtime_, generation_, media->title.c_str(), media->artist.c_str(),
          media->album.c_str(), media->artwork_type.c_str(),
          media->artwork.data(), media->artwork.size(), &control_error);
      if (result == SENDER_OK) {
        media_generation_ = generation_;
        media_revision_ = media->revision;
      } else if (result != SENDER_BACKPRESSURE) {
        std::lock_guard lock(mutex_);
        snapshot_.metadata_error =
            std::string(control_error.operation) + ": " + control_error.message;
      }
    }
    if (library_.ReceiverVolume(runtime_, generation_, &percent, &revision,
                                &control_error) == SENDER_OK &&
        revision && revision != volume_observed_revision_) {
      if (percent > 100)
        throw std::runtime_error("Receiver reported an invalid volume");
      volume_observed_revision_ = revision;
      auto record = registry_.Find(selected_);
      record.muted = percent == 0;
      if (percent) record.volume = double(percent) / 100.0;
      registry_.Update(record);
      std::lock_guard lock(mutex_);
      snapshot_.applied_volume = percent;
    }
  }
  std::optional<sender_snapshot> protocol;
  std::string failure;
  if (runtime_ && generation_) {
    sender_snapshot measured{};
    measured.size = sizeof(measured);
    measured.version = 1;
    sender_error error{};
    auto result = library_.Snapshot(runtime_, generation_, &measured, &error);
    if (result == SENDER_OK)
      protocol = measured;
    else
      failure = std::string(error.operation) + ": " + error.message;
  }
  CaptureCounters counters;
  sender_timing_info timing;
  uint64_t anchored_discontinuities{};
  auto dropped = capturing_ ? capture_->DroppedBlocks() : 0;
  {
    std::lock_guard lock(mutex_);
    snapshot_.dropped_capture_blocks = dropped;
    counters = {snapshot_.captured_frames, snapshot_.submitted_frames,
                snapshot_.nonzero_frames, snapshot_.discontinuities,
                recoveries_};
    anchored_discontinuities = anchored_discontinuities_;
    timing = snapshot_.timing;
  }
  if (!timing.effective_ms)
    timing.effective_ms =
        InitialPresentation(settings_.settings, selected_.empty()
                                                    ? ReceiverRecord{}
                                                    : registry_.Find(selected_))
            .effective_ms;
  bool recovering = desired_connected_ && session_.session &&
                    session_.session->state == SessionState::recovering;
  std::optional<CommandResult> error;
  if (session_.last_command.status == CommandStatus::failed)
    error = session_.last_command;
  auto sample = metrics_.Collect(selected_, generation_, now, protocol, timing,
                                 counters, recovering, error);
  history_.Add(sample);
  auto history = history_.Snapshot();
  auto health = health_.Evaluate(history->snapshots, now);
  auto health_level = health.level;
  auto health_detail = "health=" + health.level;
  for (auto const &reason : health.reason_codes)
    health_detail += " reason=" + reason;
  bool health_changed;
  {
    std::lock_guard lock(mutex_);
    health_changed = snapshot_.health.level != health.level ||
                     snapshot_.health.reason_codes != health.reason_codes;
    snapshot_.metrics = std::move(sample);
    snapshot_.metrics_history = std::move(history);
    snapshot_.health = std::move(health);
    snapshot_.metrics_error = std::move(failure);
  }
  if (health_changed)
    Note(health_level == "poor" || health_level == "unstable"
             ? LogLevel::warning
             : LogLevel::info,
         LogModule::streaming, LogEvent::session_changed,
         std::move(health_detail));
  last_metrics_ = now;
  auto queue_age = std::exchange(submit_age_ns_, uint64_t{0});
  auto observed = counters;
  if (observed.discontinuities < anchored_discontinuities)
    throw std::runtime_error("Anchored timeline count " +
                             std::to_string(anchored_discontinuities) +
                             " exceeds recorded discontinuities " +
                             std::to_string(observed.discontinuities));
  observed.discontinuities -= anchored_discontinuities;
  AdaptLatency(now, protocol, observed, timing, dropped, queue_age);
}
void StreamCoordinator::NoteStream(std::string code, int latency_ms) {
  if (code.empty() || latency_ms < 0)
    throw std::invalid_argument("Adaptive notice requires a code and latency");
  Note(code == "latency" ? LogLevel::info : LogLevel::warning,
       LogModule::streaming, LogEvent::adaptive_latency,
       "reason=" + code + " latency_ms=" + std::to_string(latency_ms) +
           " recovery_locked=" + std::to_string(adaptive_.RecoveryLocked()));
  AddNotice(std::move(code), {}, latency_ms);
}
void StreamCoordinator::AddNotice(std::string code, std::string detail,
                                  int latency_ms) {
  std::lock_guard lock(mutex_);
  if (notice_id_ == UINT64_MAX)
    throw std::runtime_error("Stream notice identity overflowed");
  snapshot_.notices.push_back({++notice_id_, std::move(code), latency_ms,
                               selected_, std::move(detail),
                               std::chrono::system_clock::now()});
  if (snapshot_.notices.size() > 64)
    snapshot_.notices.erase(snapshot_.notices.begin());
}
}  // namespace Win2Air
