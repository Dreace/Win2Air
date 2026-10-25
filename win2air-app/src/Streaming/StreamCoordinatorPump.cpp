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
void StreamCoordinator::Pump() {
  local_->PollChanges();
  if (shutting_down_) return;
  if (idle_disconnected_) {
    PumpIdleSource();
    return;
  }
  if (!runtime_ || !generation_) return;
  sender_error error{};
  Checked(library_.Poll(runtime_, 0, &error), error);
  if (callback_failed_) {
    callback_failed_ = false;
    throw std::runtime_error(
        "Sender callback copy or authenticated binding failed");
  }
  auto state = library_.State(runtime_, generation_);
  if (state != SENDER_READY && state != SENDER_STREAMING &&
      state != SENDER_FAILED && state != SENDER_ENDED &&
      std::chrono::steady_clock::now() >= connection_deadline_)
    throw SenderFailure("Receiver connection exceeded its deadline", 0, 0,
                        SENDER_TIMEOUT);
  if (state == SENDER_FAILED || state == SENDER_ENDED)
    throw SenderFailure(
        session_.last_command.message.empty() ? "Receiver session closed"
                                              : session_.last_command.message,
        session_.last_command.platform_code, session_.last_command.peer_code,
        session_.last_command.sender_code ? session_.last_command.sender_code
                                          : SENDER_NETWORK);
  if (returned_credentials_) {
    registry_.Authenticate(selected_, selected_);
    try {
      credentials_.Save(selected_, sender_id_, returned_credentials_->Bytes());
      Save();
      std::lock_guard lock(mutex_);
      snapshot_.credential_error.clear();
    } catch (...) {
      auto failure = DescribeFailure();
      std::lock_guard lock(mutex_);
      snapshot_.credential_error = failure.message;
    }
    returned_credentials_.reset();
  }
  if (state == SENDER_READY || state == SENDER_STREAMING) {
    // Dispatch outside the audio callback; Windows media operations run on
    // their own worker and never wait on the capture or RTP thread.
    for (unsigned count = 0; count < 16; ++count) {
      uint32_t command{};
      Checked(
          library_.TakeMediaCommand(runtime_, generation_, &command, &error),
          error);
      if (!command) break;
      if (command > 4) {
        Note(LogLevel::warning, LogModule::streaming, LogEvent::media_control,
             "outcome=invalid-command");
        continue;
      }
      constexpr char const *actions[]{"", "pause", "play", "next", "previous"};
      AddNotice(std::string("control-") + actions[command]);
      if (settings_.settings.receiver_media_control) {
        bool accepted = audio_services_.media_control
                            ? audio_services_.media_control(command)
                            : system_media_ && system_media_->Control(command);
        Note(accepted ? LogLevel::info : LogLevel::warning,
             LogModule::streaming, LogEvent::media_control,
             std::string("action=") + actions[command] +
                 (accepted ? " outcome=queued" : " outcome=rejected"));
      } else {
        Note(LogLevel::debug, LogModule::streaming, LogEvent::media_control,
             std::string("action=") + actions[command] + " outcome=disabled");
      }
    }
    if (!pending_authentication_input_.Bytes().empty()) {
      try {
        authentication_inputs_.Save(selected_, sender_id_,
                                    pending_authentication_input_.Bytes());
      } catch (...) {
        auto failure = DescribeFailure();
        std::lock_guard lock(mutex_);
        snapshot_.credential_error = failure.message;
      }
      pending_authentication_input_.Clear();
    }
    if (!capturing_) {
      auto config = session_.session->effective_presentation;
      if (config.mode == "adaptive") {
        sender_timing_info timing{};
        timing.size = sizeof(timing);
        timing.version = 1;
        Checked(library_.Timing(runtime_, generation_, &timing, &error), error);
        try {
          config = ReceiverPresentation(config, timing);
        } catch (std::invalid_argument const &failure) {
          throw SenderFailure(failure.what(), 0, 0, SENDER_CAPABILITY);
        }
        sender_presentation target{config.base_ms, config.offset_ms};
        if (config.base_ms != timing.base_ms ||
            config.offset_ms != timing.offset_ms)
          Checked(library_.RetargetPresentation(runtime_, generation_, &target,
                                                &error),
                  error);
        session_.session->effective_presentation = config;
        NoteAudio("receiver latency arrival_ms=" +
                  std::to_string(timing.receiver_render_ms) +
                  " target_ms=" + std::to_string(config.base_ms));
      }
      StartSource();
    }
    RecordConnection(true);
    if (reconnecting_) {
      if (was_connected_) ++recoveries_;
      reconnecting_ = false;
      reconnect_.Reset();
      session_.session->error.reset();
      session_.session->failure_kind.clear();
      session_.session->standby_state = recovery_standby_;
      if (recovery_standby_ == StandbyState::standby)
        FlushSource("reconnected-standby");
      session_.last_command = {};
      std::lock_guard lock(mutex_);
      snapshot_.applied_volume = EffectiveVolume(registry_.Find(selected_));
    }
    if (connecting_) {
      was_connected_ = true;
      {
        std::lock_guard lock(mutex_);
        snapshot_.applied_volume = EffectiveVolume(registry_.Find(selected_));
      }
      connecting_result_.status = CommandStatus::applied;
      connecting_result_.stage = SessionState::ready;
      session_.last_command = connecting_result_;
    }
    was_connected_ = true;
    DrainAudio();
    unsigned packets{};
    while (packets++ < 32 && !audio_.Front() && converted_.empty()) {
      std::optional<CapturedBlock> input;
      try {
        input = capture_->Pop();
      } catch (...) {
        auto failure = DescribeFailure();
        std::lock_guard lock(mutex_);
        snapshot_.source.availability = "unavailable";
        snapshot_.source.error = failure.message;
        throw;
      }
      if (!input) break;
      {
        std::lock_guard lock(mutex_);
        snapshot_.captured_frames += input->frame_count;
      }
      if ((input->flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) &&
          !converter_->DevicePositionContinues(*input)) {
        if (session_.session->standby_state == StandbyState::standby)
          ParkSource("capture-timestamp-unavailable");
        else
          FlushSource("capture-timestamp-unavailable");
        continue;
      }
      std::vector<ConvertedBlock> output;
      try {
        output = converter_->Convert(*input);
      } catch (SourceDiscontinuity const &) {
        if (session_.session->standby_state == StandbyState::standby)
          converter_->Flush(source_epoch_);
        else
          FlushSource("source-clock-jump");
        output = converter_->Convert(*input);
      }
      for (auto &block : output) {
        uint64_t nonzero{};
        for (size_t offset = 0; offset < block.pcm.size(); offset += 4)
          if (block.pcm[offset] || block.pcm[offset + 1] ||
              block.pcm[offset + 2] || block.pcm[offset + 3])
            ++nonzero;
        {
          std::lock_guard lock(mutex_);
          snapshot_.nonzero_frames += nonzero;
        }
        bool wake = silence_.Consume(block, SilenceNow());
        if (session_.session->standby_state == StandbyState::standby) {
          if (!wake) continue;
          session_.session->standby_state = StandbyState::resuming;
          Publish();
          FlushSource("standby-resume", std::chrono::seconds(2));
          auto resumed = converter_->Convert(*input);
          if (resumed.empty()) break;
          // FLUSH names the next RTP position. That packet is one silent frame;
          // the voiced audio follows it. The sender keeps the announced
          // position and anchors the sync clock to this boundary.
          constexpr uint32_t kBoundaryFrames = 352;
          constexpr uint64_t kBoundary100ns =
              uint64_t(kBoundaryFrames) * 10000000 / 44100;
          auto origin = resumed.front().capture_100ns;
          auto now_ns = audio_services_.audio_now ? audio_services_.audio_now()
                                                  : MonotonicNs();
          auto now_100ns = now_ns / 100;
          if (!origin || !now_100ns || origin > uint64_t(INT64_MAX) ||
              now_100ns > uint64_t(INT64_MAX))
            throw std::runtime_error(
                "Resume requires a capture time and a monotonic clock");
          capture_rebase_100ns_ = int64_t(now_100ns) - int64_t(origin);
          NoteAudio(
              "resume origin_100ns=" + std::to_string(origin) +
              " now_100ns=" + std::to_string(now_100ns) + " rebase_100ns=" +
              std::to_string(capture_rebase_100ns_) + " age_ms=" +
              std::to_string((int64_t(now_100ns) - int64_t(origin)) / 10000));
          ConvertedBlock lead;
          lead.source_epoch = source_epoch_;
          lead.timeline_epoch = timeline_;
          lead.first_sample = 0;
          lead.frame_count = kBoundaryFrames;
          lead.pcm.assign(size_t(kBoundaryFrames) * 4, 0);
          lead.capture_100ns =
              origin > kBoundary100ns ? origin - kBoundary100ns : origin;
          auto timeline_before = timeline_;
          if (!QueueAudio(std::move(lead)))
            throw std::runtime_error(
                "Resume boundary audio exceeds the bounded queue");
          if (timeline_ != timeline_before)
            throw std::runtime_error(
                "Resume boundary was not accepted on the flushed timeline");
          sample_shift_ = kBoundaryFrames;
          for (auto &current : resumed) {
            silence_.Consume(current, SilenceNow());
            current.timeline_epoch = timeline_;
            if (!QueueAudio(std::move(current)))
              throw std::runtime_error(
                  "Current wake audio exceeds the bounded queue");
          }
          break;
        }
        block.timeline_epoch = timeline_;
        if (!QueueAudio(std::move(block))) {
          FlushSource("conversion-queue-full");
          break;
        }
      }
    }
    if (session_.session->standby_state != StandbyState::standby &&
        silence_.Due(SilenceNow())) {
      ParkSource("silence-standby");
      session_.session->standby_state = StandbyState::standby;
    } else if (session_.session->standby_state != StandbyState::standby &&
               session_.session->standby_state != StandbyState::resuming)
      session_.session->standby_state = silence_.Pending(SilenceNow())
                                            ? StandbyState::silence_pending
                                            : StandbyState::active;
    if (session_.session->standby_state == StandbyState::standby) {
      auto now = SilenceNow();
      if (!standby_since_) standby_since_ = now;
      auto seconds = settings_.settings.standby_disconnect_seconds;
      if (seconds && !awaited_volume_ && !awaited_presentation_ &&
          now >= *standby_since_ &&
          now - *standby_since_ >= uint64_t(seconds) * 1000000000) {
        Note(LogLevel::info, LogModule::streaming, LogEvent::session_changed,
             "operation=standby-disconnected");
        CloseReceiver();
        AddNotice("standby-disconnect");
        idle_disconnected_ = true;
        session_.session->state = SessionState::disconnected;
        session_.session->generation = 0;
        {
          std::lock_guard lock(mutex_);
          snapshot_.timing = {};
          snapshot_.applied_volume.reset();
        }
        Publish();
        return;
      }
    } else {
      standby_since_.reset();
    }
    DrainAudio();
    sender_timing_info timing{};
    timing.size = sizeof(timing);
    timing.version = 1;
    Checked(library_.Timing(runtime_, generation_, &timing, &error), error);
    session_.session->effective_presentation.status =
        timing.setup_applied ? CommandStatus::applied
                             : CommandStatus::submitted;
    session_.session->effective_presentation.applied_samples =
        timing.buffer_samples;
    {
      std::lock_guard lock(mutex_);
      snapshot_.timing = timing;
    }
  }
  Publish();
  if (connecting_ && connecting_result_.status == CommandStatus::applied) {
    connecting_->set_value(connecting_result_);
    connecting_.reset();
  }
}
}  // namespace Win2Air
