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
CommandResult StreamCoordinator::ReopenStream(CommandResult result) {
  auto completion = std::make_shared<std::promise<CommandResult>>();
  auto ready = completion->get_future();
  Cleanup(false, true);
  desired_connected_ = true;
  active_intent_ = result.intent_epoch;
  recovery_standby_ = StandbyState::active;
  try {
    BeginConnection(result, {}, completion);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (ready.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
      if (result.intent_epoch != intent_epoch_ || stopping_) {
        result.status = CommandStatus::canceled;
        result.message = "Connection intent canceled";
        return result;
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw SenderFailure(
            "Reopened receiver connection exceeded its deadline", 0, 0,
            SENDER_TIMEOUT);
      Pump();
      if (ready.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto connected = ready.get();
    if (connected.status != CommandStatus::applied)
      throw SenderFailure(connected.message.empty()
                              ? "Reopened receiver connection did not start"
                              : connected.message,
                          connected.platform_code, connected.peer_code,
                          connected.sender_code);
    connected.intent_epoch = result.intent_epoch;
    return connected;
  } catch (...) {
    auto failure = DescribeFailure();
    Recover(failure);
    throw;
  }
}
bool StreamCoordinator::MovePresentation(PresentationConfig const &next) {
  if (!runtime_ || !generation_ || !session_.session) return false;
  auto current = session_.session->effective_presentation;
  auto buffer_ms = CaptureBufferMs(next);
  try {
    capture_->CheckBufferBudget(buffer_ms);
  } catch (...) {
    std::lock_guard lock(mutex_);
    snapshot_.adaptive_error = DescribeFailure().message;
    return false;
  }
  sender_presentation requested{next.base_ms, next.offset_ms};
  sender_error error{};
  if (library_.RetargetPresentation(runtime_, generation_, &requested,
                                    &error) != SENDER_OK) {
    std::lock_guard lock(mutex_);
    snapshot_.adaptive_error =
        std::string(error.operation) + ": " + error.message;
    return false;
  }
  try {
    capture_->SetBufferBudget(buffer_ms);
  } catch (...) {
    sender_presentation restore{current.base_ms, current.offset_ms};
    sender_error restore_error{};
    library_.RetargetPresentation(runtime_, generation_, &restore,
                                  &restore_error);
    std::lock_guard lock(mutex_);
    snapshot_.adaptive_error = DescribeFailure().message;
    return false;
  }
  session_.session->effective_presentation.base_ms = next.base_ms;
  session_.session->effective_presentation.offset_ms = next.offset_ms;
  session_.session->effective_presentation.effective_ms = next.effective_ms;
  sender_timing_info applied{};
  applied.size = sizeof(applied);
  applied.version = 1;
  sender_error timing_error{};
  bool timed = library_.Timing(runtime_, generation_, &applied,
                               &timing_error) == SENDER_OK;
  std::lock_guard lock(mutex_);
  if (timed) snapshot_.timing = applied;
  snapshot_.adaptive_error.clear();
  return true;
}
void StreamCoordinator::AdaptLatency(
    uint64_t now, std::optional<sender_snapshot> const &protocol,
    CaptureCounters const &counters, sender_timing_info timing,
    uint64_t dropped, uint64_t queue_age_ns) {
  if (settings_.settings.latency_mode != "adaptive" || !runtime_ ||
      !generation_ || !session_.session || !capturing_ || reconnecting_ ||
      snapshot_.presentation_pending || !protocol || !timing.setup_applied)
    return;
  constexpr auto required_counters =
      SENDER_SNAPSHOT_SEND | SENDER_SNAPSHOT_RESEND;
  if ((protocol->available & required_counters) != required_counters) {
    adaptive_.Defer(now);
    return;
  }
  auto state = session_.session->state;
  if (state != SessionState::ready && state != SessionState::streaming) return;
  if (session_.session->standby_state == StandbyState::standby ||
      session_.session->standby_state == StandbyState::resuming) {
    adaptive_.Defer(now);
    return;
  }
  if (adaptive_generation_ != generation_) {
    adaptive_.Reset(timing.base_ms, timing.offset_ms);
    adaptive_generation_ = generation_;
    adaptive_dropped_ = dropped;
  }
  LatencyObservation observation;
  observation.now_ns = now;
  observation.generation = generation_;
  observation.sent = protocol->sent_packets;
  observation.retransmitted = protocol->resend_requests;
  observation.discontinuities = counters.discontinuities;
  observation.queue_age_ms = double(queue_age_ns) / 1000000.0;
  bool failed = session_.last_command.status == CommandStatus::failed;
  observation.interrupted =
      dropped > adaptive_dropped_ ||
      (failed && session_.last_command.command_id != adaptive_failed_command_);
  adaptive_dropped_ = dropped;
  if (failed) adaptive_failed_command_ = session_.last_command.command_id;
  auto candidate = adaptive_.Observe(observation);
  if (!candidate) return;
  auto current = session_.session->effective_presentation;
  PresentationConfig next = current;
  next.base_ms = candidate->milliseconds;
  next.offset_ms = current.offset_ms;
  next.effective_ms = next.base_ms + next.offset_ms;
  if (next.base_ms < current.base_ms &&
      !capture_->HoldsQueuedAudio(CaptureBufferMs(next))) {
    adaptive_.Defer(now);
    return;
  }
  bool changed = false;
  try {
    if (next.base_ms < current.base_ms) {
      // Re-anchor a lowered RTP timeline so scheduled audio cannot overlap.
      changed = ApplyPresentation(next, active_intent_);
      if (changed) session_.session->effective_presentation = next;
    } else {
      changed = MovePresentation(next);
    }
  } catch (...) {
    std::lock_guard lock(mutex_);
    snapshot_.adaptive_error = DescribeFailure().message;
  }
  if (!changed) {
    adaptive_.Reject(now);
    if (std::string_view(candidate->reason) != "lower")
      NoteStream(candidate->reason, current.effective_ms);
    NoteStream("retarget", current.effective_ms);
    return;
  }
  adaptive_.Confirm(candidate->milliseconds, now);
  if (std::string_view(candidate->reason) != "lower")
    NoteStream(candidate->reason, next.effective_ms);
  NoteStream("latency", next.effective_ms);
}
std::future<CommandResult> StreamCoordinator::SetLatency(std::string mode,
                                                         int custom_ms) {
  return Queue(false, [this, mode = std::move(mode), custom_ms](auto result) {
    auto candidate = settings_.settings;
    candidate.latency_mode = mode;
    candidate.custom_ms = custom_ms;
    if (session_.session)
      return ChangePresentation(result, candidate,
                                registry_.Find(session_.session->receiver_id));
    RequestedPresentation(mode, custom_ms, 0);
    auto previous = settings_;
    settings_.settings = candidate;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SetSilenceTimeout(int seconds) {
  return Queue(false, [this, seconds](auto result) {
    SilenceDetector validate;
    validate.Configure(seconds, SilenceNow());
    auto previous = settings_;
    if (!idle_disconnected_ && session_.session &&
        session_.session->standby_state == StandbyState::standby &&
        seconds == 0)
      FlushSource("standby-disabled");
    settings_.settings.silence_timeout = seconds;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    silence_.Configure(seconds, SilenceNow());
    standby_since_.reset();
    if (idle_disconnected_ && seconds == 0) {
      AddNotice("standby-leave");
      Cleanup(false, true);
      recovery_standby_ = StandbyState::active;
      BeginConnection(result, {});
    }
    if (session_.session &&
        session_.session->standby_state == StandbyState::standby &&
        seconds == 0) {
      session_.session->standby_state = StandbyState::resuming;
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::SetStandbyDisconnect(
    int seconds) {
  return Queue(false, [this, seconds](auto result) {
    auto previous = settings_;
    settings_.settings.standby_disconnect_seconds = seconds;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      throw;
    }
    standby_since_.reset();
    if (idle_disconnected_ && seconds == 0) {
      AddNotice("standby-leave");
      Cleanup(false, true);
      recovery_standby_ = StandbyState::active;
      BeginConnection(result, {});
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
std::future<CommandResult> StreamCoordinator::ResetAdaptiveLatency(
    std::string identity) {
  return Queue(false, [this, identity = std::move(identity)](auto result) {
    auto current = registry_.Find(identity);
    auto cleared = current;
    cleared.latency_profile.reset();
    bool live = runtime_ && generation_ && session_.session &&
                session_.session->receiver_id == current.receiver_id &&
                (session_.session->state == SessionState::ready ||
                 session_.session->state == SessionState::streaming) &&
                settings_.settings.latency_mode == "adaptive";
    std::optional<PresentationConfig> reset_presentation;
    if (live) {
      sender_timing_info timing{};
      timing.size = sizeof(timing);
      timing.version = 1;
      sender_error error{};
      Checked(library_.Timing(runtime_, generation_, &timing, &error), error);
      reset_presentation = ReceiverPresentation(
          InitialPresentation(settings_.settings, cleared), timing);
    }
    auto previous = registry_;
    registry_.Update(cleared);
    try {
      Save();
    } catch (...) {
      registry_ = std::move(previous);
      throw;
    }
    if (reset_presentation) {
      if (!(reset_presentation->effective_ms <
                    session_.session->effective_presentation.effective_ms
                ? ApplyPresentation(*reset_presentation, result.intent_epoch)
                : MovePresentation(*reset_presentation))) {
        registry_ = previous;
        std::string message =
            "Adaptive latency reset did not move the current presentation";
        try {
          Save();
        } catch (...) {
          message += "; recorded budget restore: " + DescribeFailure().message;
        }
        throw std::runtime_error(message);
      }
      adaptive_.Reset(reset_presentation->base_ms,
                      reset_presentation->offset_ms);
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
}  // namespace Win2Air
