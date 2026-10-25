#include <fstream>
#include <future>

#include "../../src/Diagnostics/Logger.h"
#include "../../src/Streaming/CommandDispatcher.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
#include "test_support.h"
using namespace Win2Air;
void TestCommandLifecycle(wchar_t const* path) {
  SenderLibrary library(path);
  CommandDispatcher dispatcher(library);
  auto created = dispatcher.Create().get();
  Require(created.status == CommandStatus::applied,
          "Runtime creation completion");
  auto old_epoch = created.intent_epoch;
  auto disconnect = dispatcher.Disconnect();
  auto duplicate = dispatcher.Disconnect();
  auto ended = disconnect.get();
  auto repeated = duplicate.get();
  Require(ended.status == CommandStatus::applied &&
              repeated.status == CommandStatus::applied,
          "Repeated cleanup is idempotent");
  Require(
      ended.command_id < repeated.command_id && ended.intent_epoch > old_epoch,
      "Serial command identity and cancellation epoch");
  Require(dispatcher.DestroyCount() == 1 && !dispatcher.OwnsRuntime(),
          "Successful destroy clears handle exactly once");
  Require(!dispatcher.AcceptEvent(old_epoch, 0), "Old event rejected");
  auto queued = dispatcher.Submitted();
  Require(queued.status == CommandStatus::submitted,
          "Submission must remain distinct from completion");
  Require(dispatcher.Create().get().status == CommandStatus::applied,
          "Runtime can be recreated");
  Tests::ReceiverFixture receiver;
  sender_target target{};
  target.control_port = static_cast<uint16_t>(receiver.port);
  target.features = (1ULL << 9) | (1ULL << 41) | (1ULL << 48);
  target.volume_percent = 50;
  auto connection = dispatcher.Connect(target, "127.0.0.1", "AA:BB:CC:DD:EE:FF",
                                       "Controlled receiver");
  Require(
      connection.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
      "Connection failure must complete within fixture bound");
  auto failed_connection = connection.get();
  Require(failed_connection.status == CommandStatus::failed,
          "Rejected control connection cannot count as applied");
  Require(receiver.connections > 0,
          ("Real DLL did not contact the independent loopback receiver; " +
           failed_connection.message +
           "; platform=" + std::to_string(failed_connection.platform_code) +
           "; peer=" + std::to_string(failed_connection.peer_code))
              .c_str());
  auto states = dispatcher.TakeStates();
  Require(!states.empty(), "Real DLL callback crossed MSVC boundary");
  Require(dispatcher.Disconnect().get().status == CommandStatus::applied,
          "Failed receiver lifecycle releases runtime");
  Tests::AudioFixture audio;
  auto pcm = audio.Next(441);
  Require(pcm.size() == 882 && audio.sample_index == 441 &&
              audio.monotonic_ns == 1010000000,
          "Fixed PCM and clock fixture");
  Tests::EndpointFixture endpoint;
  endpoint.SetMute(true);
  Require(endpoint.muted && endpoint.volume == 0.35 && endpoint.writes == 1,
          "Independent endpoint fixture");
}
