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
std::string StreamCoordinator::ResolveSource() {
  try {
    return devices_->Resolve(settings_.settings.audio_source);
  } catch (...) {
    auto failure = DescribeFailure();
    std::lock_guard lock(mutex_);
    snapshot_.source.availability = "unavailable";
    snapshot_.source.error = failure.message;
    throw;
  }
}
void StreamCoordinator::StartSource() {
  endpoint_ = ResolveSource();
  if (source_epoch_ == UINT64_MAX)
    throw std::runtime_error("Audio source epoch exhausted");
  ++source_epoch_;
  auto const &config = session_.session->effective_presentation;
  auto buffer_ms = CaptureBufferMs(config);
  capture_->Start(endpoint_, source_epoch_, buffer_ms);
  ClearAudio();
  audio_.SetCapacity(PcmCapacity(config));
  mix_ = capture_->Format();
  converter_->Open(*reinterpret_cast<WAVEFORMATEX *>(mix_.data()),
                   source_epoch_);
  ++timeline_;
  capturing_ = true;
  silence_.Configure(settings_.settings.silence_timeout, SilenceNow());
  if (!settings_.settings.local_playback)
    local_->Disable(endpoint_, source_epoch_);
  {
    std::lock_guard lock(mutex_);
    snapshot_.source = {
        endpoint_, "available", "",
        ValidateAudioFormat(*reinterpret_cast<WAVEFORMATEX *>(mix_.data())),
        source_epoch_};
    snapshot_.capture_period = capture_->PeriodInfo();
  }
}
void StreamCoordinator::Note(LogLevel level, LogModule module, LogEvent event,
                             std::string detail, int code, int platform,
                             int peer) {
  if (!audio_services_.log_sink) return;
  try {
    LogDiagnostic(audio_services_.log_sink,
                  {level, module, event,
                   module == LogModule::streaming || module == LogModule::audio
                       ? selected_
                       : std::string{},
                   session_.session ? unsigned(session_.session->state) : 0,
                   code, platform, peer, generation_},
                  detail);
  } catch (...) {
    std::lock_guard lock(mutex_);
    snapshot_.diagnostics_error = DescribeFailure().message;
  }
}
void StreamCoordinator::NoteAudio(std::string detail) {
  if (!audio_services_.log_sink || detail.empty()) return;
  try {
    audio_services_.log_sink(
        {LogLevel::debug, LogModule::audio, LogEvent::audio_timeline, selected_,
         session_.session ? unsigned(session_.session->standby_state) : 0, 0, 0,
         0, generation_, std::move(detail)});
  } catch (...) {
    std::lock_guard lock(mutex_);
    snapshot_.diagnostics_error = DescribeFailure().message;
  }
}
void StreamCoordinator::FlushSource(std::string reason,
                                    std::chrono::milliseconds timeout) {
  sample_shift_ = 0;
  capture_rebase_100ns_ = 0;
  if (!runtime_ || !generation_) return;
  NoteAudio("flush reason=" + reason);
  ClearAudio();
  sender_error error{};
  Checked(library_.Flush(runtime_, generation_, &error), error);
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (library_.State(runtime_, generation_) == SENDER_FLUSHING) {
    Checked(library_.Poll(runtime_, 5, &error), error);
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Source FLUSH exceeded its deadline");
  }
  if (library_.State(runtime_, generation_) != SENDER_READY)
    throw std::runtime_error("Source FLUSH failed to restore a ready session");
  converter_->Flush(source_epoch_);
  silence_.Discontinuity(SilenceNow());
  ++timeline_;
  auto anchor = TimelineAnchor(reason);
  {
    std::lock_guard lock(mutex_);
    ++snapshot_.discontinuities;
    if (anchor) ++anchored_discontinuities_;
    ++snapshot_.discontinuity_reasons[reason];
  }
}
void StreamCoordinator::ParkSource(std::string reason) {
  sample_shift_ = 0;
  capture_rebase_100ns_ = 0;
  NoteAudio("park reason=" + reason);
  ClearAudio();
  converter_->Flush(source_epoch_);
  silence_.Discontinuity(SilenceNow());
  ++timeline_;
  auto anchor = TimelineAnchor(reason);
  {
    std::lock_guard lock(mutex_);
    ++snapshot_.discontinuities;
    if (anchor) ++anchored_discontinuities_;
    ++snapshot_.discontinuity_reasons[std::move(reason)];
  }
}
void StreamCoordinator::PumpIdleSource() {
  if (suspended_ || suppressed_ || !desired_connected_ ||
      active_intent_ != intent_epoch_ || shutting_down_ || stopping_)
    return;
  for (unsigned count = 0; count < 32; ++count) {
    auto input = capture_->Pop();
    if (!input) return;
    std::vector<ConvertedBlock> blocks;
    try {
      blocks = converter_->Convert(*input);
    } catch (SourceDiscontinuity const &) {
      converter_->Flush(source_epoch_);
      silence_.Discontinuity(SilenceNow());
      blocks = converter_->Convert(*input);
    }
    bool voiced = false;
    for (auto const &block : blocks)
      voiced = silence_.Consume(block, SilenceNow()) || voiced;
    if (!voiced) continue;
    if (active_intent_ != intent_epoch_) return;
    Note(LogLevel::info, LogModule::streaming, LogEvent::session_changed,
         "operation=standby-reconnect sound=detected");
    AddNotice("standby-leave");
    Cleanup(false, true);
    recovery_standby_ = StandbyState::active;
    CommandResult result;
    result.intent_epoch = active_intent_;
    BeginConnection(result, {});
    return;
  }
}
bool StreamCoordinator::QueueAudio(ConvertedBlock block) {
  DrainAudio();
  if (block.first_sample > UINT64_MAX - sample_shift_)
    throw std::runtime_error("Sample index cannot include the resume boundary");
  block.first_sample += sample_shift_;
  if (capture_rebase_100ns_ != 0) {
    if (block.capture_100ns > uint64_t(INT64_MAX))
      throw std::runtime_error("Capture timestamp is outside the resume clock");
    auto shifted = int64_t(block.capture_100ns) + capture_rebase_100ns_;
    if (shifted <= 0)
      throw std::runtime_error(
          "Resume clock rebase moved the capture time off the timeline");
    block.capture_100ns = uint64_t(shifted);
  }
  if (block.timeline_epoch != timeline_) return false;
  if (!block.source_epoch || !block.frame_count ||
      block.pcm.size() != size_t(block.frame_count) * 4)
    throw std::runtime_error("Converted batch shape or source is invalid");
  if (block.pcm.size() > 44100 * 4 - converted_bytes_)
    throw std::runtime_error("Converted batch exceeds one second of PCM");
  converted_bytes_ += block.pcm.size();
  // Keep the unsent tail when a converter batch exceeds the ring. Backpressure
  // or future capture time must not discard samples from that batch.
  converted_.push_back(std::move(block));
  DrainAudio();
  return true;
}
void StreamCoordinator::ClearAudio() {
  audio_.Clear();
  converted_.clear();
  conversion_offset_ = converted_bytes_ = 0;
}
void StreamCoordinator::DrainAudio() {
  sender_error error{};
  for (;;) {
    if (!audio_.Front()) {
      if (converted_.empty()) break;
      auto &source = converted_.front();
      if (!conversion_offset_ && source.pcm.size() <= audio_.Capacity()) {
        converted_bytes_ -= source.pcm.size();
        if (!audio_.Push(std::move(source)))
          throw std::logic_error(
              "Converted block did not fit an empty PCM ring");
        converted_.pop_front();
      } else {
        auto bytes =
            std::min(audio_.Capacity(), source.pcm.size() - conversion_offset_);
        auto frames = conversion_offset_ / 4;
        ConvertedBlock part;
        part.source_epoch = source.source_epoch;
        part.timeline_epoch = source.timeline_epoch;
        part.first_sample = source.first_sample + frames;
        part.capture_100ns = source.capture_100ns + frames * 10000000 / 44100;
        part.frame_count = static_cast<uint32_t>(bytes / 4);
        part.pcm.assign(source.pcm.begin() + conversion_offset_,
                        source.pcm.begin() + conversion_offset_ + bytes);
        if (!audio_.Push(std::move(part)))
          throw std::logic_error(
              "Converted segment did not fit an empty PCM ring");
        conversion_offset_ += bytes;
        converted_bytes_ -= bytes;
        if (conversion_offset_ == source.pcm.size()) {
          converted_.pop_front();
          conversion_offset_ = 0;
        }
      }
    }
    auto block = audio_.Front();
    auto now =
        audio_services_.audio_now ? audio_services_.audio_now() : MonotonicNs();
    auto stamp = block->capture_100ns * 100;
    auto budget =
        uint64_t(session_.session->effective_presentation.effective_ms) *
        1000000;
    auto readiness = CaptureTime(stamp, now, budget);
    if (readiness == CaptureReadiness::wait) {
      std::lock_guard lock(mutex_);
      snapshot_.future_hold_max_ns =
          std::max(snapshot_.future_hold_max_ns, stamp - now);
      break;
    }
    if (readiness == CaptureReadiness::expired) {
      FlushSource("expired-capture");
      break;
    }
    sender_timed_audio timing{sizeof(timing), 1, block->timeline_epoch,
                              block->first_sample, stamp};
    sender_audio_format format{44100, 16, 2};
    auto result = library_.Submit(runtime_, generation_, &timing, &format,
                                  block->pcm.data(), block->pcm.size(), &error);
    if (result == SENDER_BACKPRESSURE) break;
    if (result == SENDER_SYNC) {
      FlushSource(std::string("sender-clock: ") + error.message);
      break;
    }
    Checked(result, error);
    if (now >= stamp) submit_age_ns_ = std::max(submit_age_ns_, now - stamp);
    {
      std::lock_guard lock(mutex_);
      snapshot_.submitted_frames += block->frame_count;
    }
    audio_.Pop();
    Checked(library_.Poll(runtime_, 0, &error), error);
    if (callback_failed_) {
      callback_failed_ = false;
      throw std::runtime_error(
          "Sender callback copy or authenticated binding failed");
    }
    auto state = library_.State(runtime_, generation_);
    if (state == SENDER_FAILED || state == SENDER_ENDED)
      throw SenderFailure(
          session_.last_command.message.empty() ? "Receiver session closed"
                                                : session_.last_command.message,
          session_.last_command.platform_code, session_.last_command.peer_code,
          session_.last_command.sender_code ? session_.last_command.sender_code
                                            : SENDER_NETWORK);
    if (session_.session->standby_state == StandbyState::resuming)
      session_.session->standby_state = StandbyState::active;
  }
}
}  // namespace Win2Air
