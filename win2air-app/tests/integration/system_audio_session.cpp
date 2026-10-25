#include <algorithm>
#include <chrono>

#include "../../src/Streaming/ReceiverSession.h"
#include "../../src/Streaming/StreamCoordinator.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
using namespace Win2Air;
using namespace Win2Air::Tests;
namespace {
template <class Future>
CommandResult Complete(Future future) {
  Require(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
          "Serial command did not complete within its test deadline");
  return future.get();
}
void TestVirtualStreaming(std::filesystem::path const& root) {
  {
    auto audio = std::make_shared<VirtualAudioState>();
    VirtualReceiver receiver;
    Rejects(
        [&] {
          StreamCoordinator service(receiver, root / L"discovery-rejected",
                                    true, VirtualAudioServices(audio));
        },
        "Virtual audio cannot accidentally enable network discovery");
  }
  {
    auto audio = std::make_shared<VirtualAudioState>();
    VirtualReceiver receiver;
    receiver.hold_pairing = receiver.hold_setup = true;
    receiver.export_credentials = true;
    SettingsStore store(root / L"stream");
    auto saved = store.Load();
    saved.settings.revision = 1;
    saved.settings.latency_mode = "custom";
    saved.settings.custom_ms = 750;
    store.Save(saved);
    StreamCoordinator service(receiver, root / L"stream", false,
                              VirtualAudioServices(audio));
    Require(
        Complete(service.AddReceiver("127.0.0.1", 7000, 120)).status ==
            CommandStatus::applied,
        "Virtual public probe registers the actual software receiver identity");
    auto connecting = service.Connect("AA:BB:CC:DD:EE:FF");
    WaitUntil(
        [&] {
          auto s = service.Snapshot().session->session;
          return s && s->state == SessionState::pairing;
        },
        "Receiver pairing event did not project into the real coordinator");
    Require(connecting.wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready,
            "Connection submission stays pending while receiver authentication "
            "is incomplete");
    {
      std::lock_guard lock(receiver.mutex);
      receiver.hold_pairing = false;
    }
    WaitUntil(
        [&] {
          auto s = service.Snapshot().session->session;
          return s && s->state == SessionState::setting_up;
        },
        "Receiver setup event did not project into the real coordinator");
    Require(connecting.wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready,
            "Setup notification alone cannot complete a connection");
    {
      std::lock_guard lock(receiver.mutex);
      receiver.hold_setup = false;
      receiver.backpressure = 2;
    }
    Require(Complete(std::move(connecting)).status == CommandStatus::applied,
            "Connection completes after receiver readiness and capture "
            "initialization");
    auto timing = service.Snapshot().timing;
    WaitUntil([&] { return service.Snapshot().timing.setup_applied != 0; },
              "Applied presentation was not reported");
    timing = service.Snapshot().timing;
    Require(timing.base_ms == 750 && timing.offset_ms == 120 &&
                timing.effective_ms == 870 &&
                timing.setup_latency_max_samples == 38367 &&
                timing.capacity_bytes == 197568,
            "Saved initial presentation controls setup and queue budget before "
            "the connection becomes usable");
    audio->Push(NonzeroPacket(4410));
    WaitUntil(
        [&] {
          std::lock_guard lock(receiver.mutex);
          return !receiver.accepted.empty();
        },
        "Captured virtual PCM did not reach the software receiver");
    {
      std::lock_guard lock(receiver.mutex);
      Require(receiver.attempted.size() >= 3 &&
                  receiver.attempted[0].pcm == receiver.attempted[1].pcm &&
                  receiver.attempted[0].timing.sample_index ==
                      receiver.attempted[1].timing.sample_index,
              "Backpressure retries the same complete PCM and sample position "
              "without partial acceptance");
      Require(
          std::any_of(receiver.accepted.front().pcm.begin(),
                      receiver.accepted.front().pcm.end(),
                      [](auto byte) { return byte != 0; }),
          "Real conversion delivers nonzero owned PCM to the virtual receiver");
    }
    Require(Complete(service.SetLocalPlayback(false)).status ==
                CommandStatus::applied,
            "Local mute command completes after virtual endpoint acquisition");
    {
      std::lock_guard lock(audio->mutex);
      Require(audio->levels.at("virtual-default").muted,
              "Selected endpoint is muted");
    }
    size_t received;
    {
      std::lock_guard lock(receiver.mutex);
      received = receiver.accepted.size();
    }
    audio->Push(NonzeroPacket(4410));
    WaitUntil(
        [&] {
          std::lock_guard lock(receiver.mutex);
          return receiver.accepted.size() > received;
        },
        "Local endpoint mute incorrectly prevented remote PCM submission");
    {
      std::lock_guard lock(receiver.mutex);
      Require(std::any_of(receiver.accepted.back().pcm.begin(),
                          receiver.accepted.back().pcm.end(),
                          [](auto byte) { return byte != 0; }),
              "Receiver content stays nonzero while local endpoint is muted");
    }
    {
      std::lock_guard lock(audio->mutex);
      audio->levels.at("virtual-default") = {0.1f, true};
      audio->external_change = true;
    }
    auto default_epoch = service.Snapshot().source.source_epoch;
    audio->ChangeDefault("virtual-specified");
    WaitUntil(
        [&] {
          auto source = service.Snapshot().source;
          return source.endpoint_id == "virtual-specified" &&
                 source.source_epoch > default_epoch;
        },
        "Following mode did not react to the virtual default output change");
    AudioSourceConfig specified;
    specified.follow_system_default = false;
    specified.endpoint_id = "virtual-specified";
    Require(
        Complete(service.SetSource(specified)).status == CommandStatus::applied,
        "Source handover flushes and reopens the specified virtual driver");
    {
      std::lock_guard lock(audio->mutex);
      Require(audio->levels.at("virtual-default").volume == 0.65f &&
                  !audio->levels.at("virtual-default").muted &&
                  audio->levels.at("virtual-specified").muted,
              "Source handover restores the original endpoint snapshot after "
              "external changes");
    }
    {
      std::lock_guard lock(receiver.mutex);
      received = receiver.accepted.size();
      Require(receiver.flushed > 0, "Source change reaches receiver FLUSH");
    }
    audio->Push(NonzeroPacket(4800));
    WaitUntil(
        [&] {
          std::lock_guard lock(receiver.mutex);
          return receiver.accepted.size() > received;
        },
        "Specified source did not resume after handover");
    {
      std::lock_guard lock(receiver.mutex);
      Require(receiver.accepted[received].timing.sample_index == 0,
              "Source handover starts a new receiver sample anchor");
    }
    {
      std::lock_guard lock(audio->mutex);
      audio->devices[1].mix_format = WaveFormat(96000);
    }
    auto old_epoch = service.Snapshot().source.source_epoch;
    audio->Notify({"virtual-specified", DEVICE_STATE_ACTIVE, false, true});
    WaitUntil(
        [&] { return service.Snapshot().source.source_epoch > old_epoch; },
        "Virtual source format notification did not rebuild capture and "
        "conversion");
    {
      std::lock_guard lock(receiver.mutex);
      received = receiver.accepted.size();
    }
    audio->Push(NonzeroPacket(9600));
    WaitUntil(
        [&] {
          std::lock_guard lock(receiver.mutex);
          return receiver.accepted.size() > received;
        },
        "Changed input format did not reach the virtual receiver after "
        "converter reinitialization");
    Require(Complete(service.Disconnect()).status == CommandStatus::applied,
            "Disconnect waits for capture stop and receiver ownership release");
    {
      std::lock_guard lock(audio->mutex);
      Require(audio->levels.at("virtual-specified").volume == 0.25f &&
                  audio->levels.at("virtual-specified").muted &&
                  audio->opened == audio->stopped && !audio->wrong_thread,
              "Disconnect restores originally muted endpoint and joins every "
              "virtual capture worker");
    }
    Require(Complete(service.SetLocalPlayback(true)).status ==
                CommandStatus::applied,
            "Disconnected local preference can be safely restored");
    Require(Complete(service.Connect("AA:BB:CC:DD:EE:FF")).status ==
                CommandStatus::applied,
            "Reconnect uses safely copied caller-owned credentials");
    {
      std::lock_guard lock(receiver.mutex);
      Require(receiver.imported == 1 && receiver.presentations.size() == 2 &&
                  receiver.presentations.back().base_ms == 750 &&
                  receiver.presentations.back().offset_ms == 120,
              "Reconnect reapplies saved initial presentation and imports the "
              "verified credential binding");
    }
    audio->Remove("virtual-specified");
    WaitUntil(
        [&] {
          auto state = service.Snapshot();
          return state.reconnect && state.session->session &&
                 state.session->session->state == SessionState::recovering;
        },
        "Removed specified source did not stop sending and enter recovery");
    Require(service.Snapshot().session->last_command.status ==
                CommandStatus::failed,
            "Removed specified source reports failure rather than changing to "
            "another output");
    Require(service.Snapshot().source.availability == "unavailable" &&
                !service.Snapshot().source.error.empty(),
            "Removed specified endpoint remains visibly unavailable after "
            "session cleanup");
    Require(Complete(service.Shutdown()).status == CommandStatus::applied,
            "Virtual session shutdown releases all resources");
    {
      std::lock_guard lock(receiver.mutex);
      Require(receiver.created == receiver.destroyed,
              "Virtual receiver owns no leaked runtimes");
    }
  }
  {
    auto audio = std::make_shared<VirtualAudioState>();
    VirtualReceiver receiver;
    receiver.hold_pairing = true;
    StreamCoordinator service(receiver, root / L"cancel", false,
                              VirtualAudioServices(audio));
    Require(Complete(service.AddReceiver("127.0.0.1", 7000)).status ==
                CommandStatus::applied,
            "Register virtual cancellation receiver");
    auto pending = service.Connect("AA:BB:CC:DD:EE:FF");
    WaitUntil(
        [&] {
          std::lock_guard lock(receiver.mutex);
          return receiver.paired != 0;
        },
        "Pairing did not begin");
    Require(Complete(service.Disconnect()).status == CommandStatus::applied &&
                Complete(std::move(pending)).status == CommandStatus::canceled,
            "Disconnect cancels the pending authentication intent and "
            "completes actual cleanup");
    Require(Complete(service.Shutdown()).status == CommandStatus::applied,
            "Canceled connection leaves shutdown usable");
  }
  {
    auto audio = std::make_shared<VirtualAudioState>();
    VirtualReceiver receiver;
    receiver.reject_auth = true;
    StreamCoordinator service(receiver, root / L"auth-error", false,
                              VirtualAudioServices(audio));
    Require(Complete(service.AddReceiver("127.0.0.1", 7000)).status ==
                CommandStatus::applied,
            "Register controlled authentication receiver");
    auto result = Complete(service.Connect("AA:BB:CC:DD:EE:FF"));
    Require(result.status == CommandStatus::failed &&
                result.stage == SessionState::error && result.peer_code == 470,
            "Authentication rejection reaches the connection future with its "
            "actual peer status");
    Require(Complete(service.Shutdown()).status == CommandStatus::applied,
            "Authentication rejection does not retain ownership");
  }
  {
    auto audio = std::make_shared<VirtualAudioState>();
    VirtualReceiver receiver;
    receiver.export_credentials = true;
    auto path = root / L"credential-error";
    StreamCoordinator service(receiver, path, false,
                              VirtualAudioServices(audio));
    Require(Complete(service.AddReceiver("127.0.0.1", 7000)).status ==
                CommandStatus::applied,
            "Register credential persistence receiver");
    std::filesystem::create_directories(path / L"credentials" /
                                        L"AA-BB-CC-DD-EE-FF.bin");
    Require(Complete(service.Connect("AA:BB:CC:DD:EE:FF")).status ==
                CommandStatus::applied,
            "Caller credential persistence failure leaves an authenticated "
            "session usable");
    WaitUntil([&] { return !service.Snapshot().credential_error.empty(); },
              "Credential persistence error was hidden");
    {
      std::lock_guard lock(receiver.mutex);
      receiver.reject_destroy = true;
    }
    auto failed = Complete(service.Disconnect());
    Require(failed.status == CommandStatus::failed &&
                failed.platform_code == ERROR_ACCESS_DENIED,
            "Destroy failure returns the platform error and preserves the "
            "owned runtime");
    unsigned created;
    {
      std::lock_guard lock(receiver.mutex);
      created = receiver.created;
    }
    Require(
        Complete(service.Connect("AA:BB:CC:DD:EE:FF")).status ==
            CommandStatus::failed,
        "New connection is blocked while receiver cleanup remains incomplete");
    {
      std::lock_guard lock(receiver.mutex);
      Require(receiver.created == created,
              "Cleanup failure cannot create a second receiver");
      receiver.reject_destroy = false;
    }
    Require(Complete(service.Disconnect()).status == CommandStatus::applied &&
                Complete(service.Shutdown()).status == CommandStatus::applied,
            "Retry completes retained cleanup without destroying a released "
            "runtime twice");
  }
}
}  // namespace
void TestSystemAudioSession(wchar_t const* dll, wchar_t const* root) {
  ReceiverSession session;
  session.generation = 7;
  session.state = SessionState::connecting;
  Require(!ApplySenderStep(session, 6, SENDER_STEP_PAIRING) &&
              session.state == SessionState::connecting,
          "Stale authentication step cannot change the selected session");
  Require(ApplySenderStep(session, 7, SENDER_STEP_PAIRING) &&
              session.state == SessionState::pairing,
          "Actual authentication notification projects pairing");
  Require(ApplySenderStep(session, 7, SENDER_STEP_SETTING_UP) &&
              session.state == SessionState::setting_up,
          "Actual configuration notification projects setting up");
  ApplySenderState(session, 7, SENDER_READY, nullptr);
  Require(session.state == SessionState::ready,
          "Ready follows the actual protocol state");
  AppSettings settings;
  ReceiverRecord receiver;
  receiver.offset_ms = 120;
  auto config = InitialPresentation(settings, receiver);
  Require(config.base_ms == 1000 && config.effective_ms == 1120,
          "Initial balanced budget includes saved receiver compensation");
  settings.latency_mode = "custom";
  settings.custom_ms = 750;
  receiver.offset_ms = -100;
  Require(InitialPresentation(settings, receiver).effective_ms == 650,
          "Custom saved budget is validated before creation");
  settings.custom_ms = 100;
  Rejects([&] { InitialPresentation(settings, receiver); },
          "Effective budget below receiver minimum rejected");
  SenderLibrary library(dll);
  auto audio = std::make_shared<VirtualAudioState>();
  StreamCoordinator coordinator(library, root, false,
                                VirtualAudioServices(audio));
  auto initial = coordinator.Snapshot();
  Require(
      initial.local_playback && !initial.discovering && initial.session,
      "Offline coordinator publishes honest initial state without browsing");
  auto missing = coordinator.Connect("AA:BB:CC:DD:EE:FF");
  Require(
      missing.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
          missing.get().status == CommandStatus::failed,
      "Unavailable selected identity fails without creating a session");
  auto first = coordinator.Disconnect(), second = coordinator.Disconnect();
  Require(
      first.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
          second.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
      "Repeated disconnect commands resolve");
  Require(second.get().status == CommandStatus::applied &&
              !coordinator.Snapshot().session->session,
          "Cleanup completes before reporting disconnected");
  auto invalid = coordinator.AddReceiver("127.0.0.1", 0);
  Require(
      invalid.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
          invalid.get().status == CommandStatus::failed,
      "Invalid manual endpoint rejects without leaking the probe runtime");
  Require(coordinator.Disconnect().get().status == CommandStatus::applied,
          "Cleanup after rejected probe preserves ownership");
  AudioSourceConfig unavailable;
  unavailable.follow_system_default = false;
  unavailable.endpoint_id = "missing-render-endpoint";
  auto missing_source = coordinator.SetSource(unavailable);
  Require(missing_source.wait_for(std::chrono::seconds(2)) ==
                  std::future_status::ready &&
              missing_source.get().status == CommandStatus::failed,
          "Virtual source failure resolves the command instead of terminating "
          "its worker");
  Require(
      coordinator.SetLocalPlayback(true).get().status == CommandStatus::applied,
      "Persist the initial playback preference before storage failure "
      "injection");
  {
    auto path = std::filesystem::path(root) / L"settings.json";
    winrt::handle blocked{CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr)};
    Require(static_cast<bool>(blocked),
            "Block settings replacement during the command");
    auto changed = coordinator.SetLocalPlayback(false).get();
    Require(changed.status == CommandStatus::failed &&
                coordinator.Snapshot().local_playback,
            "Failed settings commit restores the previous local playback "
            "preference");
    auto sources = coordinator.Snapshot().sources;
    Require(
        !sources.empty(),
        "Virtual source rollback test requires its configured render endpoint");
    AudioSourceConfig specified;
    specified.follow_system_default = false;
    specified.endpoint_id = sources.front().id;
    auto source_change = coordinator.SetSource(specified).get();
    Require(source_change.status == CommandStatus::failed &&
                coordinator.Snapshot().source_config.follow_system_default,
            "Failed source commit preserves the previous selection instead of "
            "publishing an applied source");
  }
  auto exit = coordinator.Shutdown(), repeated_exit = coordinator.Shutdown();
  Require(exit.get().status == CommandStatus::applied &&
              exit.get().command_id == repeated_exit.get().command_id,
          "Repeated exit shares the actual completed cleanup operation");
  Require(coordinator.Connect("AA:BB:CC:DD:EE:FF").get().status ==
              CommandStatus::canceled,
          "Shutdown rejects new connection intent");
  TestVirtualStreaming(std::filesystem::path(root) / L"virtual-sessions");
  {
    auto latency_root = std::filesystem::path(root) / L"immediate-dispatch";
    auto dispatch_audio = std::make_shared<VirtualAudioState>();
    VirtualReceiver dispatch_receiver;
    SettingsStore latency_store(latency_root);
    auto dispatch_settings = latency_store.Load();
    dispatch_settings.settings.revision = 1;
    dispatch_settings.settings.latency_mode = "ultra";
    latency_store.Save(dispatch_settings);
    StreamCoordinator stream(dispatch_receiver, latency_root, false,
                             VirtualAudioServices(dispatch_audio));
    Require(Complete(stream.AddReceiver("127.0.0.1", 7000)).status ==
                    CommandStatus::applied &&
                Complete(stream.Connect("AA:BB:CC:DD:EE:FF")).status ==
                    CommandStatus::applied,
            "Immediate dispatch uses an isolated virtual capture and receiver");
    dispatch_audio->Push(NonzeroPacket(441));
    WaitUntil([&] { return stream.Snapshot().timing.next_sample != 0; },
              "Captured PCM did not pass through conversion and submission");
    {
      std::lock_guard lock(dispatch_receiver.mutex);
      Require(dispatch_receiver.dispatched_frames != 0 &&
                  dispatch_receiver.pending_transport_frames == 0 &&
                  dispatch_receiver.timing_with_pending_audio == 0,
              "Accepted audio reaches transport in the same pump before "
              "publishing timing or returning to the idle wait");
      Require(
          dispatch_receiver.startup_ms == 0,
          "Realtime mode sends complete RTP frames without startup prefill");
    }
    Require(Complete(stream.Disconnect()).status == CommandStatus::applied,
            "Immediate dispatch preserves bounded session cleanup");
  }
}
