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
uint64_t StreamCoordinator::RecoveryNow() const {
  return audio_services_.recovery_now ? audio_services_.recovery_now()
                                      : MonotonicNs();
}
void StreamCoordinator::Recover(FailureDetail const &failure) {
  auto disposition = ClassifyRecovery(failure.sender_code, failure.peer);
  if (disposition == RecoveryDisposition::authentication &&
      authentication_input_used_ && !selected_.empty()) {
    pending_authentication_input_.Clear();
    auto file = selected_;
    std::replace(file.begin(), file.end(), ':', '-');
    if (std::filesystem::exists(root_ / L"authentication" / (file + ".bin"))) {
      try {
        authentication_inputs_.Delete(selected_);
      } catch (...) {
        auto detail = DescribeFailure();
        std::lock_guard lock(mutex_);
        snapshot_.credential_error = detail.message;
      }
    }
  }
  auto previous = session_.session;
  if (previous && previous->state != SessionState::recovering && !reconnecting_)
    recovery_standby_ = previous->standby_state == StandbyState::standby
                            ? StandbyState::standby
                            : StandbyState::active;
  Fail(failure.message, failure.platform, failure.peer, failure.sender_code);
  bool retry = disposition == RecoveryDisposition::retry &&
               desired_connected_ && !suppressed_ &&
               settings_.settings.auto_reconnect &&
               active_intent_ == intent_epoch_ && !stopping_ && !shutting_down_;
  reconnecting_ = false;
  try {
    Cleanup(!retry, true);
  } catch (...) {
    desired_connected_ = false;
    reconnect_.Reset();
    auto message = failure.message + "; cleanup: " + DescribeFailure().message;
    try {
      local_->Restore();
    } catch (...) {
      message += "; local restore: " + DescribeFailure().message;
    }
    Fail(std::move(message));
    return;
  }
  if (!session_.session && !selected_.empty()) {
    ReceiverSession session;
    session.receiver_id = selected_;
    session.intent_epoch = intent_epoch_;
    session_.session = std::move(session);
  }
  if (session_.session) {
    session_.session->generation = 0;
    session_.session->effective_presentation.status = CommandStatus::submitted;
    session_.session->effective_presentation.applied_samples = 0;
    session_.session->error = session_.last_command;
    session_.session->failure_kind =
        disposition == RecoveryDisposition::authentication ? "authentication"
        : disposition == RecoveryDisposition::retry        ? "transient"
                                                           : "terminal";
  }
  if (retry) {
    reconnect_.Schedule(RecoveryNow(), intent_epoch_, failure.message);
    Note(LogLevel::warning, LogModule::streaming, LogEvent::recovery_changed,
         "operation=scheduled", failure.sender_code, failure.platform,
         failure.peer);
    session_.session->state = SessionState::recovering;
    session_.session->standby_state = recovery_standby_;
  } else {
    desired_connected_ = false;
    reconnect_.Reset();
    Note(LogLevel::warning, LogModule::streaming, LogEvent::recovery_changed,
         disposition == RecoveryDisposition::authentication
             ? "operation=authentication-required"
             : "operation=stopped",
         failure.sender_code, failure.platform, failure.peer);
    if (session_.session) session_.session->state = SessionState::error;
  }
  Publish();
}
void StreamCoordinator::TickRecovery() {
  if (!desired_connected_ || suppressed_ || suspended_ || shutting_down_ ||
      stopping_ || !settings_.settings.auto_reconnect)
    return;
  if (!reconnect_.Due(RecoveryNow(), intent_epoch_)) return;
  Note(LogLevel::info, LogModule::streaming, LogEvent::recovery_changed,
       "operation=retry-started");
  reconnect_.Cancel();
  CommandResult result;
  result.intent_epoch = intent_epoch_;
  try {
    ResolveSource();
    Cleanup(false, true);
    reconnecting_ = true;
    BeginConnection(result, {});
  } catch (...) {
    Recover(DescribeFailure());
  }
}
void StreamCoordinator::NotifySystemEvent(SystemEvent event,
                                          unsigned interface_index) noexcept {
  if (shutting_down_ || stopping_) return;
  {
    std::lock_guard lock(mutex_);
    auto bit = 1u << static_cast<unsigned>(event);
    if (event == SystemEvent::network_changed)
      changed_interface_ =
          (system_events_.load() & bit) && changed_interface_ != interface_index
              ? 0
              : interface_index;
    system_events_.fetch_or(bit);
  }
  wake_.notify_one();
}
void StreamCoordinator::AutoConnectSelected() {
  if (desired_connected_ || suppressed_ || suspended_ || runtime_ ||
      session_.session || selected_.empty() || shutting_down_ || stopping_)
    return;
  auto record = registry_.Find(selected_);
  if (!record.auto_connect || !record.connect_established ||
      (!record.manual_endpoint && record.discovered_endpoints.empty()))
    return;
  desired_connected_ = true;
  active_intent_ = intent_epoch_;
  recovery_standby_ = StandbyState::active;
  CommandResult result;
  result.intent_epoch = active_intent_;
  try {
    BeginConnection(result, {});
  } catch (...) {
    Recover(DescribeFailure());
  }
}
void StreamCoordinator::SystemEvents() {
  unsigned events, index;
  {
    std::lock_guard lock(mutex_);
    events = system_events_.exchange(0);
    index = changed_interface_.exchange(0);
  }
  if (!events) return;
  if (events & (1u << static_cast<unsigned>(SystemEvent::suspend))) {
    suspended_ = true;
    if (idle_disconnected_) {
      capture_->Stop();
      capturing_ = false;
      local_->Restore();
    } else if (desired_connected_ && runtime_) {
      Recover({"System suspended", 0, 0, SENDER_NETWORK});
    }
    reconnect_.Cancel();
  }
  if (events & (1u << static_cast<unsigned>(SystemEvent::resume))) {
    bool was_suspended = suspended_;
    suspended_ = false;
    if (was_suspended && idle_disconnected_ && desired_connected_) {
      StartSource();
    } else if (was_suspended && desired_connected_ && !runtime_ &&
               settings_.settings.auto_reconnect)
      reconnect_.Resume(RecoveryNow(), intent_epoch_, "System resumed");
  }
  if (events & (1u << static_cast<unsigned>(SystemEvent::network_changed))) {
    if (desired_connected_ && runtime_ && !suspended_) {
      auto record = registry_.Find(selected_);
      auto endpoint = record.manual_endpoint ? record.manual_endpoint
                      : record.discovered_endpoints.empty()
                          ? std::nullopt
                          : std::optional(record.discovered_endpoints.front());
      if (!index || !endpoint || !endpoint->interface_index ||
          endpoint->interface_index == index)
        Recover({"Network interface changed", 0, 0, SENDER_NETWORK});
    }
    AutoConnectSelected();
  }
  Publish();
}
std::future<CommandResult> StreamCoordinator::SetAutoReconnect(bool enabled) {
  return Queue(!enabled, [this, enabled](auto result) {
    auto previous = settings_;
    settings_.settings.auto_reconnect = enabled;
    try {
      Save();
    } catch (...) {
      settings_ = std::move(previous);
      if (desired_connected_) {
        active_intent_ = intent_epoch_;
        if (session_.session) session_.session->intent_epoch = intent_epoch_;
      }
      if (auto waiting = reconnect_.Snapshot()) {
        reconnect_.Resume(RecoveryNow(), intent_epoch_, waiting->last_reason);
      }
      throw;
    }
    if (!enabled) {
      bool pending = reconnect_.Snapshot().has_value() || reconnecting_ ||
                     (session_.session &&
                      session_.session->state == SessionState::recovering);
      reconnect_.Reset();
      reconnecting_ = false;
      if (pending) {
        desired_connected_ = false;
        Cleanup();
      } else if (desired_connected_) {
        active_intent_ = result.intent_epoch;
        if (session_.session)
          session_.session->intent_epoch = result.intent_epoch;
      }
    }
    result.status = CommandStatus::applied;
    return result;
  });
}
}  // namespace Win2Air
