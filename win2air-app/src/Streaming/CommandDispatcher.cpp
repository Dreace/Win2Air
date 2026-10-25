#include "CommandDispatcher.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
namespace Win2Air {
namespace {
CommandResult Failed(CommandResult result, sender_error const& error) {
  result.status = CommandStatus::failed;
  result.platform_code = error.platform_code;
  result.peer_code = error.peer_code;
  result.message =
      "Sender operation failed; code=" + std::to_string(error.code) +
      ", platform=" + std::to_string(error.platform_code) +
      ", peer=" + std::to_string(error.peer_code);
  result.stage = SessionState::error;
  return result;
}
}  // namespace
CommandDispatcher::CommandDispatcher(SenderLibrary& library)
    : library_(library), worker_([this] { Run(); }) {}
CommandDispatcher::~CommandDispatcher() {
  if (Disconnect().get().status != CommandStatus::applied) std::terminate();
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_one();
  worker_.join();
}
void SENDER_CALL CommandDispatcher::OnState(
    void* context, uint64_t generation, sender_state state,
    sender_error const* error) noexcept {
  auto& self = *static_cast<CommandDispatcher*>(context);
  try {
    StateEvent copy{self.runtime_epoch_,
                    generation,
                    state,
                    error ? int(error->code) : 0,
                    error ? error->platform_code : 0,
                    error ? error->peer_code : 0};
    std::lock_guard lock(self.events_mutex_);
    if (self.states_.size() >= 256 || self.completion_states_.size() >= 256)
      throw std::runtime_error("State callback queue exceeded bound");
    self.completion_states_.push_back(copy);
    self.states_.push_back(copy);
  } catch (...) {
    self.callback_failed_ = true;
  }
}
void SENDER_CALL CommandDispatcher::OnCredentials(
    void* context, uint64_t generation,
    sender_credentials const* credentials) noexcept {
  auto& self = *static_cast<CommandDispatcher*>(context);
  try {
    if (!credentials || !credentials->receiver_id || !credentials->material)
      throw std::runtime_error("Credential callback input absent");
    size_t size = strnlen_s(credentials->material, 65537);
    if (!size || size > 65536)
      throw std::runtime_error("Credential callback material invalid");
    CredentialEvent copy;
    copy.intent_epoch = self.runtime_epoch_;
    copy.generation = generation;
    copy.sender_id = credentials->sender_id;
    copy.receiver_id = credentials->receiver_id;
    copy.material = SecureBuffer(
        {reinterpret_cast<uint8_t const*>(credentials->material), size});
    std::lock_guard lock(self.events_mutex_);
    if (self.credentials_.size() >= 4)
      throw std::runtime_error("Credential callback queue exceeded bound");
    self.credentials_.push_back(std::move(copy));
  } catch (...) {
    self.callback_failed_ = true;
  }
}
std::future<CommandResult> CommandDispatcher::Enqueue(
    bool cancel, std::function<CommandResult(CommandResult)> operation) {
  auto promise = std::make_shared<std::promise<CommandResult>>();
  auto future = promise->get_future();
  std::lock_guard lock(mutex_);
  if (stopping_) throw std::runtime_error("Command dispatcher is stopping");
  CommandResult result;
  result.command_id = ++next_command_;
  result.intent_epoch = epoch_;
  result.generation = generation_;
  if (queue_.size() >= 256) {
    result.status = CommandStatus::rejected;
    result.message = "Command queue exceeds bound";
    submitted_ = result;
    promise->set_value(result);
    return future;
  }
  if (cancel) result.intent_epoch = ++epoch_;
  submitted_ = result;
  queue_.push_back(
      [this, promise, result, operation = std::move(operation)]() mutable {
        CommandResult finished;
        try {
          finished = operation(result);
        } catch (...) {
          finished = result;
          finished.status = CommandStatus::failed;
          finished.stage = SessionState::error;
          finished.message = "Command failed at native service boundary";
        }
        Publish(finished);
        if (finished.status == CommandStatus::submitted) {
          pending_connect_ = promise;
          pending_result_ = finished;
        } else
          promise->set_value(finished);
      });
  wake_.notify_one();
  return future;
}
void CommandDispatcher::Publish(CommandResult const& result) {
  auto snapshot = std::make_shared<SessionSnapshot>();
  snapshot->revision = ++revision_;
  snapshot->intent.intent_epoch = epoch_;
  snapshot->intent.selected_receiver_id = selected_receiver_;
  snapshot->intent.desired_connected = desired_connected_;
  snapshot->intent.suppressed_this_run = suppressed_this_run_;
  snapshot->last_command = result;
  if (!active_receiver_.empty() && runtime_) {
    ReceiverSession session;
    session.receiver_id = active_receiver_;
    session.generation = generation_;
    session.intent_epoch = runtime_epoch_;
    session.state = result.stage;
    session.last_command = result;
    if (result.status == CommandStatus::failed) session.error = result;
    snapshot->session = std::move(session);
  }
  std::lock_guard lock(mutex_);
  snapshot_ = std::move(snapshot);
}
std::future<CommandResult> CommandDispatcher::Create() {
  return Enqueue(false, [this](CommandResult result) {
    if (result.intent_epoch != epoch_) {
      result.status = CommandStatus::canceled;
      return result;
    }
    if (runtime_) {
      result.status = CommandStatus::rejected;
      result.message = "Runtime already owned";
      return result;
    }
    sender_create_options options{
        SENDER_ABI_VERSION,
        sizeof(sender_create_options),
        {1, "Win2Air", "Win2Air", 44100 * 4 * 2, OnState, OnCredentials, this},
        {1000, 0},
        nullptr};
    sender_error error{};
    if (library_.Create(options, &runtime_, &error) != SENDER_OK)
      return Failed(result, error);
    runtime_epoch_ = result.intent_epoch;
    owns_runtime_ = true;
    callback_failed_ = false;
    result.status = CommandStatus::applied;
    return result;
  });
}
CommandResult CommandDispatcher::Cleanup(CommandResult result) {
  CompleteConnect(CommandStatus::canceled);
  if (!runtime_) {
    result.status = CommandStatus::applied;
    return result;
  }
  sender_error error{};
  if (generation_) {
    auto end = library_.End(runtime_, generation_, &error);
    if (end != SENDER_OK && end != SENDER_ALREADY_ENDED)
      return Failed(result, error);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      auto status = library_.Destroy(runtime_, &error);
      if (status == SENDER_OK) {
        runtime_ = nullptr;
        owns_runtime_ = false;
        generation_ = 0;
        active_receiver_.clear();
        ++destroy_count_;
        result.status = CommandStatus::applied;
        return result;
      }
      if (status != SENDER_STATE) return Failed(result, error);
      library_.Poll(runtime_, 10, &error);
    }
    return Failed(result, error);
  }
  if (library_.Destroy(runtime_, &error) != SENDER_OK)
    return Failed(result, error);
  runtime_ = nullptr;
  owns_runtime_ = false;
  generation_ = 0;
  active_receiver_.clear();
  ++destroy_count_;
  result.status = CommandStatus::applied;
  return result;
}
std::future<CommandResult> CommandDispatcher::Disconnect() {
  return Enqueue(true, [this](CommandResult result) {
    desired_connected_ = false;
    suppressed_this_run_ = true;
    return Cleanup(result);
  });
}
std::future<CommandResult> CommandDispatcher::Connect(sender_target target,
                                                      std::string address,
                                                      std::string identity,
                                                      std::string name) {
  return Enqueue(true, [this, target, address = std::move(address),
                        identity = std::move(identity),
                        name = std::move(name)](CommandResult result) mutable {
    if (result.intent_epoch != epoch_) {
      result.status = CommandStatus::canceled;
      return result;
    }
    selected_receiver_ = identity;
    desired_connected_ = true;
    suppressed_this_run_ = false;
    if (runtime_ && generation_) {
      auto cleanup = Cleanup(result);
      if (cleanup.status != CommandStatus::applied) return cleanup;
    }
    if (result.intent_epoch != epoch_) {
      result.status = CommandStatus::canceled;
      return result;
    }
    if (!runtime_) {
      sender_create_options options{SENDER_ABI_VERSION,
                                    sizeof(sender_create_options),
                                    {1, "Win2Air", "Win2Air", 44100 * 4 * 2,
                                     OnState, OnCredentials, this},
                                    {1000, 0},
                                    nullptr};
      sender_error error{};
      if (library_.Create(options, &runtime_, &error) != SENDER_OK)
        return Failed(result, error);
      owns_runtime_ = true;
    }
    runtime_epoch_ = result.intent_epoch;
    target.address = address.c_str();
    target.receiver_id = identity.c_str();
    target.name = name.c_str();
    sender_error error{};
    uint64_t generation{};
    sender_audio_format format{44100, 16, 2};
    auto status = library_.Connect(runtime_, &target, &format, nullptr, nullptr,
                                   &generation, &error);
    if (status != SENDER_OK) return Failed(result, error);
    generation_ = generation;
    active_receiver_ = identity;
    result.generation = generation;
    result.status = CommandStatus::submitted;
    result.stage = SessionState::connecting;
    return result;
  });
}
void CommandDispatcher::Run() {
  for (;;) {
    std::function<void()> work;
    {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(10),
                     [this] { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) return;
      if (!queue_.empty()) {
        work = std::move(queue_.front());
        queue_.pop_front();
      }
    }
    if (work) work();
    if (runtime_) {
      sender_error error{};
      auto result = library_.Poll(runtime_, 0, &error);
      std::deque<StateEvent> completions;
      {
        std::lock_guard lock(events_mutex_);
        completions.swap(completion_states_);
      }
      for (auto const& event : completions)
        if (AcceptEvent(event.intent_epoch, event.generation)) {
          if (event.state == SENDER_READY || event.state == SENDER_STREAMING)
            CompleteConnect(CommandStatus::applied, &event);
          else if (event.state == SENDER_FAILED)
            CompleteConnect(CommandStatus::failed, &event);
        }
      if (result != SENDER_OK || callback_failed_) {
        CommandResult failure;
        failure.intent_epoch = epoch_;
        failure.generation = generation_;
        failure.status = CommandStatus::failed;
        failure.stage = SessionState::error;
        failure.platform_code = error.platform_code;
        failure.peer_code = error.peer_code;
        failure.message = callback_failed_
                              ? "Callback copy failed or queue exceeded bound"
                              : "Protocol poll failed";
        Publish(failure);
        CompleteConnect(CommandStatus::failed);
        callback_failed_ = false;
      }
    }
  }
}
void CommandDispatcher::CompleteConnect(CommandStatus status,
                                        StateEvent const* event) {
  if (!pending_connect_) return;
  auto result = pending_result_;
  result.status = status;
  if (event) {
    result.platform_code = event->platform_code;
    result.peer_code = event->peer_code;
  }
  result.stage = status == CommandStatus::applied  ? SessionState::ready
                 : status == CommandStatus::failed ? SessionState::error
                                                   : SessionState::disconnected;
  result.message =
      status == CommandStatus::failed     ? "Receiver connection failed"
      : status == CommandStatus::canceled ? "Connection intent canceled"
                                          : "Receiver connection completed";
  if (event && status == CommandStatus::failed)
    result.message += "; sender code=" + std::to_string(event->code);
  Publish(result);
  pending_connect_->set_value(result);
  pending_connect_.reset();
}
bool CommandDispatcher::AcceptEvent(uint64_t epoch,
                                    uint64_t generation) const noexcept {
  return epoch == epoch_ && generation == generation_ && owns_runtime_;
}
CommandResult CommandDispatcher::Submitted() {
  std::lock_guard lock(mutex_);
  return submitted_;
}
ImmutableSessionSnapshot CommandDispatcher::Snapshot() {
  std::lock_guard lock(mutex_);
  return snapshot_;
}
std::deque<CredentialEvent> CommandDispatcher::TakeCredentials() {
  std::lock_guard lock(events_mutex_);
  std::deque<CredentialEvent> result;
  while (!credentials_.empty()) {
    auto event = std::move(credentials_.front());
    credentials_.pop_front();
    if (AcceptEvent(event.intent_epoch, event.generation))
      result.push_back(std::move(event));
  }
  return result;
}
std::deque<StateEvent> CommandDispatcher::TakeStates() {
  std::lock_guard lock(events_mutex_);
  std::deque<StateEvent> result;
  while (!states_.empty()) {
    auto event = states_.front();
    states_.pop_front();
    if (AcceptEvent(event.intent_epoch, event.generation))
      result.push_back(event);
  }
  return result;
}
}  // namespace Win2Air
