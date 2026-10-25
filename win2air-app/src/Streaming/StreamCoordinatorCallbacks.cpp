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
void SENDER_CALL StreamCoordinator::VolumeCompleted(
    void *context, sender_command_completion const *completion) noexcept {
  auto &self = *static_cast<StreamCoordinator *>(context);
  if (!completion || completion->size != sizeof(*completion) ||
      completion->version != 1 || completion->generation != self.generation_ ||
      self.active_intent_ != self.intent_epoch_)
    return;
  if (completion->kind == SENDER_COMMAND_VOLUME &&
      completion->command_id == self.awaited_volume_)
    self.volume_completion_ = *completion;
  if (completion->kind == SENDER_COMMAND_PRESENTATION &&
      completion->command_id == self.awaited_presentation_)
    self.presentation_completion_ = *completion;
  if (completion->kind == SENDER_COMMAND_METADATA) {
    try {
      self.metadata_transport_error_ =
          completion->result == SENDER_OK ? "" : completion->error.message;
      std::lock_guard lock(self.mutex_);
      self.snapshot_.metadata_error = self.metadata_transport_error_;
    } catch (...) {
      self.callback_failed_ = true;
    }
  }
}
uint64_t StreamCoordinator::SilenceNow() const {
  return audio_services_.silence_now ? audio_services_.silence_now()
                                     : MonotonicNs();
}
bool StreamCoordinator::ApplyPresentation(PresentationConfig const &config,
                                          uint64_t epoch) {
  if (!runtime_ || !generation_ || !session_.session || !capturing_)
    throw std::runtime_error(
        "Presentation requires an active ready receiver and source");
  capture_->CheckBufferBudget(CaptureBufferMs(config));
  if (volume_sequence_ == UINT64_MAX)
    throw std::runtime_error("Control command identity exhausted");
  awaited_presentation_ = ++volume_sequence_;
  presentation_completion_.reset();
  sender_presentation requested{config.base_ms, config.offset_ms};
  sender_error error{};
  auto finish = [this] {
    awaited_presentation_ = 0;
    presentation_completion_.reset();
    std::lock_guard lock(mutex_);
    snapshot_.presentation_pending = false;
  };
  auto reanchor = [this] {
    capture_->Clear();
    ClearAudio();
    sample_shift_ = 0;
    capture_rebase_100ns_ = 0;
    converter_->Flush(source_epoch_);
    silence_.Discontinuity(SilenceNow());
    ++timeline_;
  };
  try {
    Checked(library_.SetPresentation(runtime_, generation_,
                                     awaited_presentation_, &requested, &error),
            error);
    {
      std::lock_guard lock(mutex_);
      snapshot_.presentation_pending = true;
    }
    Publish();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!presentation_completion_) {
      if (epoch != intent_epoch_ || stopping_) {
        finish();
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Presentation response exceeded its deadline");
      Checked(library_.Poll(runtime_, 5, &error), error);
    }
    auto completion = *presentation_completion_;
    Checked(completion.result, completion.error);
    if (completion.presentation.base_ms != config.base_ms ||
        completion.presentation.offset_ms != config.offset_ms)
      throw std::runtime_error(
          "Receiver completion differs from the requested presentation");
    sender_timing_info applied{};
    applied.size = sizeof(applied);
    applied.version = 1;
    Checked(library_.Timing(runtime_, generation_, &applied, &error), error);
    if (!applied.setup_applied || applied.base_ms != config.base_ms ||
        applied.offset_ms != config.offset_ms)
      throw std::runtime_error(
          "Receiver timing did not confirm the requested presentation");
    {
      std::lock_guard lock(mutex_);
      snapshot_.timing = applied;
    }
    finish();
    reanchor();
    capture_->SetBufferBudget(CaptureBufferMs(config));
    audio_.SetCapacity(PcmCapacity(config));
    return true;
  } catch (...) {
    auto original = std::current_exception();
    finish();
    try {
      reanchor();
    } catch (...) {
      throw std::runtime_error(
          DescribeFailure(original).message +
          "; source reanchor: " + DescribeFailure().message);
    }
    std::rethrow_exception(original);
  }
}
CommandResult StreamCoordinator::ChangePresentation(CommandResult result,
                                                    AppSettings settings,
                                                    ReceiverRecord record) {
  auto config = RequestedPresentation(settings.latency_mode, settings.custom_ms,
                                      record.offset_ms);
  auto previous = settings_;
  auto previous_registry = registry_;
  auto checked_registry = registry_;
  checked_registry.Update(record);
  auto checked_settings = settings_;
  checked_settings.settings = settings;
  checked_settings.receivers = checked_registry.Records();
  ValidateSettings(checked_settings);
  bool current = runtime_ && capturing_ && session_.session &&
                 session_.session->receiver_id == record.receiver_id;
  if (current && config.mode == "adaptive" &&
      session_.session->effective_presentation.mode == "adaptive") {
    sender_timing_info timing{};
    timing.size = sizeof(timing);
    timing.version = 1;
    sender_error error{};
    Checked(library_.Timing(runtime_, generation_, &timing, &error), error);
    config = ReceiverPresentation(config, timing);
  }
  auto old = current ? session_.session->effective_presentation : config;
  bool reopen =
      current && (config.mode != old.mode || config.base_ms != old.base_ms);
  if (reopen) {
    try {
      capture_->CheckBufferBudget(CaptureBufferMs(config));
    } catch (...) {
      result.status = CommandStatus::failed;
      result.message = DescribeFailure().message;
      result.generation = generation_;
      return result;
    }
  } else if (current && !(config.effective_ms < old.effective_ms
                              ? ApplyPresentation(config, result.intent_epoch)
                              : MovePresentation(config))) {
    std::string message;
    {
      std::lock_guard lock(mutex_);
      message = std::move(snapshot_.adaptive_error);
      snapshot_.adaptive_error.clear();
    }
    if (message.empty())
      throw std::runtime_error(
          "Live presentation change did not move the sender timeline");
    result.status = CommandStatus::failed;
    result.message = std::move(message);
    result.generation = generation_;
    return result;
  }
  try {
    settings_.settings = std::move(settings);
    registry_.Update(record);
    Save();
  } catch (...) {
    auto detail = DescribeFailure();
    settings_ = std::move(previous);
    registry_ = std::move(previous_registry);
    if (current && !reopen && result.intent_epoch == intent_epoch_) {
      if (!MovePresentation(old)) {
        std::string rollback;
        {
          std::lock_guard lock(mutex_);
          rollback = std::move(snapshot_.adaptive_error);
          snapshot_.adaptive_error.clear();
        }
        detail.message += "; presentation rollback: " + rollback;
      } else {
        adaptive_.Reset(old.base_ms, old.offset_ms);
        adaptive_generation_ = generation_;
      }
    }
    throw SenderFailure(detail.message, detail.platform, detail.peer);
  }
  if (reopen) return ReopenStream(std::move(result));
  if (current) {
    config.status = CommandStatus::applied;
    session_.session->effective_presentation = config;
    adaptive_.Reset(config.base_ms, config.offset_ms);
    adaptive_generation_ = generation_;
  }
  result.status = CommandStatus::applied;
  result.generation = current ? generation_ : 0;
  return result;
}
bool StreamCoordinator::ApplyVolume(unsigned percent, uint64_t epoch) {
  if (!runtime_ || !generation_ || !session_.session)
    throw std::runtime_error("Remote volume requires an active session");
  if (volume_sequence_ == UINT64_MAX)
    throw std::runtime_error("Remote volume command sequence exhausted");
  awaited_volume_ = ++volume_sequence_;
  volume_completion_.reset();
  sender_error error{};
  Checked(library_.SetVolume(runtime_, generation_, awaited_volume_, percent,
                             &error),
          error);
  {
    std::lock_guard lock(mutex_);
    snapshot_.volume_pending = true;
  }
  Publish();
  auto finish = [this] {
    awaited_volume_ = 0;
    volume_completion_.reset();
    std::lock_guard lock(mutex_);
    snapshot_.volume_pending = false;
  };
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  try {
    while (!volume_completion_) {
      if (epoch != intent_epoch_ || stopping_) {
        finish();
        return false;
      }
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(
            "Receiver volume response exceeded its deadline");
      Checked(library_.Poll(runtime_, 5, &error), error);
      Pump();
    }
    auto completion = *volume_completion_;
    Checked(completion.result, completion.error);
    {
      std::lock_guard lock(mutex_);
      snapshot_.applied_volume = percent;
    }
    finish();
    return true;
  } catch (...) {
    finish();
    throw;
  }
}
CommandResult StreamCoordinator::ChangeVolume(CommandResult result,
                                              ReceiverRecord record) {
  auto percent = EffectiveVolume(record);
  auto previous_registry = registry_;
  auto current = runtime_ && capturing_ && session_.session &&
                 session_.session->receiver_id == record.receiver_id;
  auto previous_percent =
      current ? EffectiveVolume(registry_.Find(record.receiver_id)) : 0;
  if (current && !ApplyVolume(percent, result.intent_epoch)) {
    result.status = CommandStatus::canceled;
    return result;
  }
  try {
    registry_.Update(std::move(record));
    Save();
  } catch (...) {
    auto detail = DescribeFailure();
    registry_ = std::move(previous_registry);
    if (current) {
      try {
        if (!ApplyVolume(previous_percent, result.intent_epoch))
          detail.message += "; remote rollback canceled by new intent";
      } catch (...) {
        detail.message += "; remote rollback: " + DescribeFailure().message;
      }
    }
    throw SenderFailure(detail.message, detail.platform, detail.peer);
  }
  result.status = CommandStatus::applied;
  result.generation = current ? generation_ : 0;
  result.stage = current ? session_.session->state : SessionState::disconnected;
  return result;
}
void StreamCoordinator::Fail(std::string message, int platform, int peer,
                             int code) {
  session_.last_command.status = CommandStatus::failed;
  Note(LogLevel::error, LogModule::streaming, LogEvent::protocol_failed,
       "operation=session-failed error=" + message, code, platform, peer);
  session_.last_command.stage = SessionState::error;
  session_.last_command.message = std::move(message);
  session_.last_command.platform_code = platform;
  session_.last_command.peer_code = peer;
  session_.last_command.sender_code = code;
  if (session_.session) {
    session_.session->state = SessionState::error;
    session_.session->error = session_.last_command;
  }
  if (connecting_) {
    connecting_result_.status = CommandStatus::failed;
    connecting_result_.stage = SessionState::error;
    connecting_result_.message = session_.last_command.message;
    connecting_result_.platform_code = platform;
    connecting_result_.peer_code = peer;
    connecting_result_.sender_code = code;
    connecting_->set_value(connecting_result_);
    connecting_.reset();
  }
  Publish();
}
void SENDER_CALL StreamCoordinator::State(void *context, uint64_t generation,
                                          sender_state state,
                                          sender_error const *error) noexcept {
  auto &self = *static_cast<StreamCoordinator *>(context);
  if (generation != self.generation_ ||
      self.active_intent_ != self.intent_epoch_ || !self.session_.session)
    return;
  try {
    ApplySenderState(*self.session_.session, generation, state, error);
    if (state == SENDER_FAILED && self.session_.session->error)
      self.session_.last_command = *self.session_.session->error;
  } catch (...) {
    self.callback_failed_ = true;
  }
}
void SENDER_CALL StreamCoordinator::Step(void *context, uint64_t generation,
                                         sender_step step) noexcept {
  auto &self = *static_cast<StreamCoordinator *>(context);
  if (self.active_intent_ == self.intent_epoch_ && self.session_.session)
    ApplySenderStep(*self.session_.session, generation, step);
}
void SENDER_CALL
StreamCoordinator::Credentials(void *context, uint64_t generation,
                               sender_credentials const *value) noexcept {
  auto &self = *static_cast<StreamCoordinator *>(context);
  try {
    if (generation != self.generation_ ||
        self.active_intent_ != self.intent_epoch_ || !value)
      return;
    if (!value->receiver_id || self.selected_ != value->receiver_id ||
        value->sender_id != self.sender_id_ || !value->material ||
        strnlen(value->material, 193) != 192)
      throw std::runtime_error(
          "Authenticated credentials have an invalid binding");
    self.returned_credentials_.emplace(std::span<uint8_t const>(
        reinterpret_cast<uint8_t const *>(value->material), 192));
  } catch (...) {
    self.callback_failed_ = true;
  }
}
}  // namespace Win2Air
