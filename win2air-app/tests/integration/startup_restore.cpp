#include "../../src/App/StateRestore.h"
#include "../../src/Settings/StorageIO.h"
#include "../../src/Shell/StartupService.h"
#include "../../src/Streaming/StreamCoordinator.h"
#include "../../src/UI/ViewModels/NavigationViewModel.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
using namespace Win2Air;
using namespace Win2Air::Tests;
namespace {
template <class Future>
auto Complete(Future future) {
  Require(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "Lifecycle operation exceeded its completion deadline");
  return future.get();
}
struct Startup final : StartupBackend {
  StartupState state{StartupState::disabled};
  unsigned enables{}, disables{};
  bool reject{}, fail{};
  StartupState Read() override {
    if (fail) throw std::runtime_error("Virtual startup permission error");
    return state;
  }
  StartupState Enable() override {
    ++enables;
    if (!reject) state = StartupState::enabled;
    return state;
  }
  StartupState Disable() override {
    ++disables;
    state = StartupState::disabled;
    return state;
  }
};
SettingsSnapshot Saved(std::filesystem::path const& root) {
  SettingsStore store(root);
  auto saved = store.Load();
  saved.settings.revision = 1;
  saved.settings.selected_receiver_id = "AA:BB:CC:DD:EE:FF";
  saved.settings.restore_previous = true;
  saved.settings.minimized = true;
  saved.settings.startup = true;
  saved.settings.latency_mode = "custom";
  saved.settings.custom_ms = 750;
  saved.settings.audio_source.follow_system_default = false;
  saved.settings.audio_source.endpoint_id = "virtual-specified";
  ReceiverRecord receiver;
  receiver.receiver_id = saved.settings.selected_receiver_id;
  receiver.name = "Saved virtual receiver";
  receiver.model = "Virtual";
  receiver.identity_verified = true;
  receiver.muted = true;
  receiver.offset_ms = 120;
  receiver.features =
      (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
  receiver.connect_established = true;
  receiver.manual_endpoint = ReceiverEndpoint{"127.0.0.1", "127.0.0.1", 7000};
  saved.receivers = {receiver};
  store.Save(saved);
  uint64_t sender = 88;
  StorageIO::AtomicWrite(
      root / L"sender-identity.bin",
      {reinterpret_cast<uint8_t const*>(&sender), sizeof(sender)});
  CredentialStore(root / L"credentials")
      .Save(receiver.receiver_id, sender,
            SecureBuffer(std::string(192, 'A')).Bytes());
  return saved;
}
}  // namespace
void TestStartupRestore(wchar_t const* directory) {
  {
    auto begin = std::chrono::steady_clock::now();
    auto deadline = begin + std::chrono::milliseconds(50);
    std::mutex mutex;
    std::condition_variable finished;
    unsigned canceled{}, waited{};
    std::vector<BrowseCancellation> queries;
    auto cancel = [&] {
      ++canceled;
      return DWORD(ERROR_SUCCESS);
    };
    auto wait = [&](BrowseDeadline limit) {
      Require(canceled == 2 && limit == deadline,
              "Every browse cancellation is submitted before waiting, using "
              "the same remaining exit budget");
      ++waited;
      std::unique_lock lock(mutex);
      return finished.wait_until(lock, limit, [] { return false; });
    };
    queries = {{cancel, wait}, {cancel, wait}};
    std::this_thread::sleep_until(begin + std::chrono::milliseconds(40));
    Rejects([&] { CancelBrowses(queries, deadline); },
            "Missing browse cancellation callbacks preserve timeout failure");
    Require(waited == 2 && std::chrono::steady_clock::now() - begin <
                               std::chrono::seconds(1),
            "Browse cleanup shares the budget already consumed by earlier "
            "cleanup instead of accumulating independent waits");
  }
  auto backend = std::make_shared<Startup>();
  StartupService startup(backend);
  Require(Complete(startup.Query()).state == StartupState::disabled &&
              backend->enables == 0,
          "Reading startup defaults never enables system registration");
  Require(Complete(startup.SetEnabled(true)).applied && backend->enables == 1,
          "Startup is applied only after system acceptance");
  for (auto blocked :
       {StartupState::disabled_by_user, StartupState::disabled_by_policy}) {
    backend->state = blocked;
    auto result = Complete(startup.SetEnabled(true));
    Require(!result.applied && result.state == blocked &&
                backend->enables == 1 && !result.error.empty(),
            "System refusal remains visible and cannot be bypassed");
  }
  backend->state = StartupState::enabled_by_policy;
  Require(
      !Complete(startup.SetEnabled(false)).applied && backend->disables == 0,
      "Administrator-enabled startup is not overridden");
  backend->state = StartupState::disabled;
  backend->reject = true;
  Require(!Complete(startup.SetEnabled(true)).applied,
          "Successful API call without enabling cannot claim applied");
  backend->fail = true;
  Require(Complete(startup.Query()).state == StartupState::error,
          "System permission error is preserved");
  backend->fail = false;
  backend->reject = false;
  backend->state = StartupState::disabled;
  auto rollback = Complete(startup.SetEnabled(true, [](bool) {
    throw std::runtime_error("Virtual preference save failure");
  }));
  Require(
      !rollback.applied && rollback.state == StartupState::disabled &&
          backend->disables == 1 &&
          rollback.error.find("preference save failure") != std::string::npos,
      "Preference failure rolls accepted system startup back to its prior "
      "state and preserves the reason");
  auto native_failure = Complete(startup.SetEnabled(
      true, [](bool) { winrt::throw_hresult(E_ACCESSDENIED); }));
  Require(!native_failure.applied &&
              native_failure.state == StartupState::disabled &&
              native_failure.error.find("HRESULT=") != std::string::npos,
          "Native preference error also rolls system startup back and retains "
          "its platform result");
  StartupService unpackaged(NativeStartupBackend(false));
  Require(Complete(unpackaged.Query()).state == StartupState::unavailable,
          "Unpackaged startup is unavailable without registry workarounds");
  auto root = std::filesystem::path(directory);
  auto saved = Saved(root);
  ActivationRequest launch;
  launch.startup = true;
  auto plan = PlanRestore(saved, launch);
  Require(plan.automatic && plan.minimized &&
              plan.receiver_id == saved.settings.selected_receiver_id &&
              plan.presentation->base_ms == 750 &&
              plan.presentation->offset_ms == 120,
          "Startup restoration plans one saved receiver with actual initial "
          "budget and offset");
  VirtualReceiver receiver;
  auto audio = std::make_shared<VirtualAudioState>();
  {
    StreamCoordinator service(receiver, root, false,
                              VirtualAudioServices(audio));
    Require(Complete(service.Connect(*plan.receiver_id)).status ==
                CommandStatus::applied,
            "Restoration waits for real coordinator readiness and capture "
            "initialization");
    Require(receiver.imported == 1 &&
                receiver.presentations.back().base_ms == 750 &&
                receiver.presentations.back().offset_ms == 120 &&
                receiver.initial_volumes.back() == 0 &&
                service.Snapshot().source.endpoint_id == "virtual-specified",
            "Saved authorization, source, mute and presentation are applied to "
            "the virtual receiver");
    audio->Push(NonzeroPacket(4800));
    WaitUntil(
        [&] {
          std::lock_guard lock(receiver.mutex);
          return !receiver.accepted.empty();
        },
        "Restored virtual session did not submit current source audio");
    Require(Complete(service.SetLifecycle(false, true, true)).status ==
                CommandStatus::applied,
            "Lifecycle preferences use the serial settings writer");
    Require(
        Complete(service.SaveWindow({-5000, 5000, 960, 640, true})).status ==
            CommandStatus::applied,
        "Window placement persists without changing audio preferences");
    auto begin = std::chrono::steady_clock::now();
    Require(
        Complete(service.Shutdown(true,
                                  WindowPlacement{-2000, 2000, 960, 640, true}))
                    .status == CommandStatus::applied &&
            std::chrono::steady_clock::now() - begin < std::chrono::seconds(5),
        "Final exit stops capture and receiver within the lifecycle deadline");
    Require(Complete(service.Disconnect()).status == CommandStatus::canceled,
            "Terminated service rejects late commands");
  }
  auto restored = SettingsStore(root).Load();
  Require(!restored.settings.close_to_tray &&
              restored.settings.custom_ms == 750 &&
              restored.receivers.front().offset_ms == 120 &&
              restored.settings.window_placement.saved &&
              restored.settings.window_placement.x == -2000 &&
              restored.settings.window_placement.y == 2000 &&
              restored.receivers.front().connect_established,
          "Restart retains lifecycle, placement and presentation without stale "
          "overwrites");
  auto invalid = restored;
  invalid.settings.custom_ms = 5000;
  invalid.receivers.front().offset_ms = 1000;
  Rejects([&] { PlanRestore(invalid, launch); },
          "Invalid effective budget refuses automatic restoration");
  auto disconnected = ParseActivation(L"--disconnect");
  Require(!PlanRestore(saved, disconnected).receiver_id,
          "Explicit disconnect suppresses startup restoration");
  auto automatic = saved;
  automatic.settings.restore_previous = false;
  automatic.receivers.front().auto_connect = true;
  auto alternative = automatic.receivers.front();
  alternative.receiver_id = "11:22:33:44:55:66";
  automatic.receivers.push_back(alternative);
  Require(PlanRestore(automatic, launch).receiver_id ==
              automatic.settings.selected_receiver_id,
          "Automatic connection restores only the selected receiver even when "
          "another saved receiver also enables automatic connection");
  automatic.settings.selected_receiver_id.clear();
  Require(!PlanRestore(automatic, launch).receiver_id,
          "Automatic connection never guesses a destination without a saved "
          "selection");
  auto unsuccessful = saved;
  unsuccessful.receivers.front().connect_established = false;
  unsuccessful.receivers.front().auto_connect = true;
  Require(!PlanRestore(unsuccessful, launch).receiver_id,
          "Startup leaves a device whose last connection did not succeed "
          "disconnected");
  ActivationRequest named;
  named.receiver_id = unsuccessful.receivers.front().receiver_id;
  Require(PlanRestore(unsuccessful, named).receiver_id == named.receiver_id,
          "An explicit device selection still connects after a failed attempt");
  {
    auto failed_root = root / L"unavailable-source";
    Saved(failed_root);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    source->fail_open = true;
    StreamCoordinator service(peer, failed_root, false,
                              VirtualAudioServices(source));
    Require(
        Complete(service.Connect(saved.settings.selected_receiver_id)).status ==
                CommandStatus::failed &&
            peer.accepted.empty(),
        "Failed saved source cannot claim restoration or submit audio");
    Complete(service.Shutdown(true));
    auto failed_saved = SettingsStore(failed_root).Load();
    Require(!failed_saved.receivers.front().connect_established &&
                !PlanRestore(failed_saved, launch).receiver_id,
            "A failed connection is recorded and is not restored at startup");
  }
  {
    auto reset_root = root / L"reset-adaptive";
    auto configuration = Saved(reset_root);
    ++configuration.settings.revision;
    configuration.settings.latency_mode = "adaptive";
    auto& record = configuration.receivers.front();
    record.offset_ms = 0;
    record.firmware_version = "virtual-firmware";
    record.manual_endpoint->interface_index = 1;
    record.connect_established = true;
    record.latency_profile =
        LatencyProfile{"virtual-firmware", "127.0.0.1:7000|if=1|scope=0", 20};
    SettingsStore(reset_root).Save(configuration);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator service(peer, reset_root, false,
                              VirtualAudioServices(source));
    Require(Complete(service.Connect(record.receiver_id)).status ==
                    CommandStatus::applied &&
                service.Snapshot().timing.base_ms == 150,
            "Adaptive mode starts from the receiver report instead of the "
            "recorded budget");
    Require(
        Complete(service.ResetAdaptiveLatency(record.receiver_id)).status ==
                CommandStatus::applied &&
            service.Snapshot().timing.base_ms == 150 &&
            !SettingsStore(reset_root).Load().receivers.front().latency_profile,
        "Resetting a device clears its recorded adaptive budget and "
        "returns the current session to the receiver budget with safety "
        "margin");
    Complete(service.Shutdown(true));
  }
  NavigationViewModel navigation;
  navigation.Navigate(AppPage::statistics);
  Require(navigation.Page() == AppPage::statistics,
          "Statistics navigation does not create another audio service");
  Rejects([&] { navigation.Navigate(static_cast<AppPage>(999)); },
          "Unsupported navigation fails explicitly");
  {
    auto recovery_root = root / L"crash-recovery";
    Saved(recovery_root);
    auto source = std::make_shared<VirtualAudioState>();
    auto services = VirtualAudioServices(source);
    auto original = source->levels.at("virtual-default");
    {
      auto control = services.local_playback(recovery_root / L"recovery");
      control->Disable("virtual-default", 1);
    }
    Require(source->levels.at("virtual-default").muted,
            "Virtual crash leaves a persisted owned mute to recover");
    StorageIO::AtomicText(recovery_root / L"settings.json", "invalid settings");
    VirtualReceiver peer;
    Rejects(
        [&] {
          StreamCoordinator service(peer, recovery_root, false, services);
        },
        "Corrupted settings fail startup rather than selecting defaults");
    Require(
        source->levels.at("virtual-default").muted == original.muted &&
            source->levels.at("virtual-default").volume == original.volume &&
            !std::filesystem::exists(recovery_root / L"recovery" /
                                     L"local-playback-lease.json"),
        "Crash recovery restores the owned endpoint before configuration "
        "failure and removes the completed lease");
  }
  {
    auto failed_root = root / L"final-cleanup-failure";
    Saved(failed_root);
    VirtualReceiver peer;
    auto source = std::make_shared<VirtualAudioState>();
    StreamCoordinator service(peer, failed_root, false,
                              VirtualAudioServices(source));
    Require(
        Complete(service.Connect(saved.settings.selected_receiver_id)).status ==
            CommandStatus::applied,
        "Prepare final-exit cleanup failure");
    {
      std::lock_guard lock(peer.mutex);
      peer.reject_destroy = true;
    }
    Require(Complete(service.Shutdown(true)).status == CommandStatus::failed,
            "Final exit preserves explicit cleanup failure rather than "
            "claiming success");
    Require(Complete(service.Shutdown(true)).status == CommandStatus::failed,
            "Repeated final exit retains the completed failure result without "
            "repeating cleanup");
    Require(
        Complete(service.Connect(saved.settings.selected_receiver_id)).status ==
            CommandStatus::canceled,
        "Cleanup failure on final exit never reopens a new session");
  }
}
