#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include "../Settings/CredentialStore.h"
#include "SenderLibrary.h"
#include "SessionModels.h"
namespace Win2Air {
struct CredentialEvent {
  uint64_t intent_epoch{}, generation{}, sender_id{};
  std::string receiver_id;
  SecureBuffer material;
};
struct StateEvent {
  uint64_t intent_epoch{}, generation{};
  sender_state state{};
  int code{}, platform_code{}, peer_code{};
};
class CommandDispatcher {
  SenderLibrary& library_;
  sender_runtime* runtime_{};
  std::atomic<bool> owns_runtime_{};
  std::atomic<uint64_t> epoch_{1}, generation_{}, destroy_count_{};
  std::atomic<bool> callback_failed_{};
  uint64_t next_command_{}, revision_{}, runtime_epoch_{};
  std::mutex mutex_, events_mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> queue_;
  std::deque<StateEvent> states_;
  std::deque<StateEvent> completion_states_;
  std::deque<CredentialEvent> credentials_;
  bool stopping_{};
  ImmutableSessionSnapshot snapshot_;
  CommandResult submitted_;
  std::shared_ptr<std::promise<CommandResult>> pending_connect_;
  CommandResult pending_result_;
  std::string selected_receiver_, active_receiver_;
  bool desired_connected_{}, suppressed_this_run_{};
  std::thread worker_;
  static void SENDER_CALL OnState(void*, uint64_t, sender_state,
                                  sender_error const*) noexcept;
  static void SENDER_CALL OnCredentials(void*, uint64_t,
                                        sender_credentials const*) noexcept;
  std::future<CommandResult> Enqueue(
      bool cancel, std::function<CommandResult(CommandResult)> operation);
  CommandResult Cleanup(CommandResult result);
  void Run();
  void Publish(CommandResult const& result);
  void CompleteConnect(CommandStatus status, StateEvent const* event = nullptr);

 public:
  explicit CommandDispatcher(SenderLibrary& library);
  ~CommandDispatcher();
  CommandDispatcher(CommandDispatcher const&) = delete;
  std::future<CommandResult> Create();
  std::future<CommandResult> Disconnect();
  std::future<CommandResult> Connect(sender_target target, std::string address,
                                     std::string identity, std::string name);
  bool AcceptEvent(uint64_t epoch, uint64_t generation) const noexcept;
  bool OwnsRuntime() const noexcept { return owns_runtime_.load(); }
  uint64_t DestroyCount() const noexcept { return destroy_count_.load(); }
  CommandResult Submitted();
  ImmutableSessionSnapshot Snapshot();
  std::deque<CredentialEvent> TakeCredentials();
  std::deque<StateEvent> TakeStates();
};
}  // namespace Win2Air
