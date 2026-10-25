#include "../../src/Streaming/CommandDispatcher.h"
#include "test_support.h"
using namespace Win2Air;
void TestBorrowedCallbacks(wchar_t const* path) {
  SenderLibrary fixture(path);
  CommandDispatcher dispatcher(fixture);
  Require(dispatcher.Create().get().status == CommandStatus::applied,
          "Controlled runtime created through MSVC ABI");
  sender_target target{};
  target.control_port = 7000;
  target.features = (1ULL << 9) | (1ULL << 41) | (1ULL << 48);
  target.volume_percent = 50;
  auto connection = dispatcher.Connect(target, "127.0.0.1", "AA:BB:CC:DD:EE:FF",
                                       "Controlled callback fixture");
  Require(connection.wait_for(std::chrono::seconds(3)) ==
                  std::future_status::ready &&
              connection.get().status == CommandStatus::applied,
          "Actual ready callback completes submitted command");
  auto credentials = dispatcher.TakeCredentials();
  Require(credentials.size() == 1 &&
              credentials.front().material.Bytes().size() == 192,
          "Borrowed credential copied across DLL before source wipe");
  for (auto value : credentials.front().material.Bytes())
    Require(value == 'a', "Copied borrowed credential remains intact");
  auto old_epoch = credentials.front().intent_epoch,
       old_generation = credentials.front().generation;
  Require(dispatcher.Disconnect().get().status == CommandStatus::applied,
          "Controlled callback runtime cleanup");
  Require(!dispatcher.AcceptEvent(old_epoch, old_generation),
          "Canceled callback cannot reference old runtime");
  Require(
      dispatcher.TakeCredentials().empty() && dispatcher.TakeStates().empty(),
      "Stale callbacks discarded");
  auto invalid_target = target;
  invalid_target.features = 0;
  Require(dispatcher
                  .Connect(invalid_target, "127.0.0.1", "11:22:33:44:55:66",
                           "Unsupported controlled receiver")
                  .get()
                  .status == CommandStatus::failed,
          "Invalid capability fails explicitly");
  Require(!dispatcher.Snapshot()->session &&
              dispatcher.Snapshot()->intent.selected_receiver_id ==
                  "11:22:33:44:55:66",
          "Failed new selection does not invent an active session");
  dispatcher.Disconnect().get();
  std::atomic<bool> stop{};
  std::thread observer([&] {
    while (!stop) {
      dispatcher.TakeStates();
      std::this_thread::yield();
    }
  });
  bool completed = true;
  for (unsigned i = 0; i < 24 && completed; ++i) {
    auto result = dispatcher.Connect(target, "127.0.0.1", "AA:BB:CC:DD:EE:FF",
                                     "Concurrent state observer");
    completed =
        result.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (completed) completed = result.get().status == CommandStatus::applied;
    dispatcher.TakeCredentials();
    dispatcher.Disconnect().get();
  }
  stop = true;
  observer.join();
  Require(completed, "State observer must not steal command completion events");
}
