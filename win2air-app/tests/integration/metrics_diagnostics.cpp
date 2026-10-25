#include "../../src/UI/ViewModels/StatisticsViewModel.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
using namespace Win2Air;
using namespace Win2Air::Tests;
namespace {
struct DiscoveryState {
  std::atomic<unsigned> starts{};
  std::atomic<uint64_t> epoch{};
  std::atomic<bool> block_stop{}, waiting{}, release{true}, fail_start{};
};
class VirtualDiscovery final : public DnsSdBrowser {
  std::shared_ptr<DiscoveryState> state_;

 public:
  explicit VirtualDiscovery(std::shared_ptr<DiscoveryState> state)
      : state_(std::move(state)) {}
  void Start(uint64_t epoch) override {
    ++state_->starts;
    if (state_->fail_start.exchange(false))
      throw std::runtime_error("Virtual discovery restart failed");
    state_->epoch = epoch;
  }
  void Stop(BrowseDeadline deadline) override {
    if (!state_->block_stop.exchange(false)) return;
    state_->waiting = true;
    while (!state_->release && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    state_->waiting = false;
    if (!state_->release)
      throw std::runtime_error("Virtual discovery stop timed out");
  }
  std::vector<ServiceAnnouncement> Take() override { return {}; }
};
struct ReleaseDiscovery {
  std::shared_ptr<DiscoveryState> state;
  ~ReleaseDiscovery() { state->release = true; }
};
}  // namespace
void TestMetricsDiagnostics(wchar_t const* directory) {
  MetricsSnapshot previous, current;
  previous.receiver_id = current.receiver_id = "AA:BB:CC:DD:EE:FF";
  previous.generation = current.generation = 1;
  previous.sample_time = 1000000000;
  current.sample_time = 2000000000;
  previous.resend_requests.value = 10;
  current.resend_requests.value = 12;
  previous.resend_requests.availability = current.resend_requests.availability =
      Availability::measured;
  Require(ResendRequestsPerSecond(previous, current) == 2.0,
          "Retransmission rates use counter differences over elapsed time");
  current.resend_requests.value = 10;
  Require(ResendRequestsPerSecond(previous, current) == 0.0,
          "An observed unchanged counter produces a known zero rate");
  current.resend_requests.value = 9;
  Require(!ResendRequestsPerSecond(previous, current),
          "A counter reset does not appear as a negative rate");
  current.resend_requests.value = 12;
  current.generation = 2;
  Require(!ResendRequestsPerSecond(previous, current),
          "Separate sessions never form a continuous rate");
  current.generation = 1;
  current.sample_time = 3000000000;
  Require(!ResendRequestsPerSecond(previous, current),
          "Missing sampling windows remain unknown");
  current.sample_time = 2000000000;
  current.resend_requests.value.reset();
  Require(!ResendRequestsPerSecond(previous, current),
          "Missing counters remain unknown rather than showing zero");
  auto root = std::filesystem::path(directory);
  SettingsStore store(root);
  auto saved = store.Load();
  saved.settings.revision = 1;
  ReceiverRecord record;
  record.receiver_id = "AA:BB:CC:DD:EE:FF";
  record.name = record.model = "Virtual metrics receiver";
  record.features =
      (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
  record.manual_endpoint = ReceiverEndpoint{"127.0.0.1", "127.0.0.1", 7000};
  saved.receivers = {record};
  store.Save(saved);
  {
    auto refresh_root = root / L"receiver-refresh";
    SettingsStore(refresh_root).Save(saved);
    auto refresh_state = std::make_shared<DiscoveryState>();
    auto refresh_audio = std::make_shared<VirtualAudioState>();
    VirtualReceiver refresh_receiver;
    StreamCoordinator refresh_service(
        refresh_receiver, refresh_root, true,
        VirtualAudioServices(refresh_audio),
        std::make_unique<VirtualDiscovery>(refresh_state));
    ReleaseDiscovery release_discovery{refresh_state};
    Require(refresh_service.Connect(record.receiver_id).get().status ==
                CommandStatus::applied,
            "Connect a virtual receiver before refreshing discovery");
    auto refresh_generation =
        refresh_service.Snapshot().session->session->generation;
    refresh_state->release = false;
    refresh_state->block_stop = true;
    Require(refresh_service.RefreshReceivers().get().status ==
                CommandStatus::applied,
            "Refresh starts in the background");
    WaitUntil([&] { return refresh_state->waiting.load(); },
              "Virtual discovery stop did not start");
    Require(refresh_service.RefreshReceivers().get().status ==
                CommandStatus::applied,
            "Repeated refresh reuses the pending discovery operation");
    refresh_audio->Push(NonzeroPacket(441));
    WaitUntil(
        [&] { return refresh_service.Snapshot().submitted_frames >= 441; },
        "Audio must continue while discovery cancellation is waiting");
    Require(refresh_state->waiting &&
                refresh_service.Snapshot().session->session->generation ==
                    refresh_generation,
            "Refreshing discovery keeps the active audio session");
    refresh_state->release = true;
    WaitUntil(
        [&] {
          return refresh_state->starts == 2 &&
                 !refresh_service.Snapshot().discovery_refreshing;
        },
        "Discovery did not resume after cancellation completed");
    Require(refresh_state->epoch == 2,
            "Refresh uses a new discovery observation epoch");
    refresh_state->fail_start = true;
    refresh_service.RefreshReceivers().get();
    WaitUntil(
        [&] {
          return refresh_service.Snapshot().discovery_error ==
                 "Virtual discovery restart failed";
        },
        "Discovery failures must be visible without failing playback");
    refresh_service.RefreshReceivers().get();
    WaitUntil(
        [&] {
          return refresh_state->epoch == 4 &&
                 !refresh_service.Snapshot().discovery_refreshing;
        },
        "A failed discovery refresh remains retryable");
    Require(refresh_service.Snapshot().discovery_error.empty(),
            "Successful refresh clears its earlier error");
  }
  auto audio = std::make_shared<VirtualAudioState>();
  VirtualReceiver receiver;
  std::atomic<uint64_t> clock{MonotonicNs()};
  auto services = VirtualAudioServices(audio);
  services.metrics_now = services.recovery_now = [&] { return clock.load(); };
  std::atomic<unsigned> safe_events{};
  std::atomic<bool> fail_logs{};
  std::atomic<bool> poor_health_logged{};
  std::atomic<bool> logs_flushed{};
  services.log_flush = [&] { logs_flushed = true; };
  services.log_sink = [&](LogRecord const& event) {
    Require(event.receiver.empty() || ValidReceiverIdentity(event.receiver),
            "Log events identify a receiver only through its public identity");
    ++safe_events;
    if (fail_logs) throw std::runtime_error("Controlled log write failure");
    if (event.event == LogEvent::session_changed &&
        event.level >= LogLevel::info &&
        event.detail.starts_with("health=poor"))
      poor_health_logged = true;
  };
  StreamCoordinator service(receiver, root, false, services);
  Require(service.Connect(record.receiver_id).get().status ==
              CommandStatus::applied,
          "Establish a virtual metrics session with the real capture and "
          "conversion chain");
  clock += 1000000000;
  WaitUntil([&] { return service.Snapshot().metrics.generation != 0; },
            "Live metrics never observed the active generation");
  audio->Push(NonzeroPacket(4410));
  WaitUntil([&] { return service.Snapshot().submitted_frames >= 4410; },
            "Current virtual audio did not reach the receiver");
  auto before = service.Snapshot().metrics.sample_time;
  clock += 1000000000;
  WaitUntil([&] { return service.Snapshot().metrics.sample_time > before; },
            "Metrics were not collected at the next monotonic sample");
  auto measured = service.Snapshot();
  Require(
      measured.metrics.sent_packets.value &&
          *measured.metrics.sent_packets.value > 0 &&
          measured.metrics.submitted_frames.value == 4410 &&
          measured.metrics.nonzero_frames.value == 4410 &&
          !measured.metrics.clock_lock.value &&
          measured.health.level == "unknown" && safe_events > 0,
      "Measured frame and packet counts never imply a measured receiver clock");
  {
    std::lock_guard lock(receiver.mutex);
    receiver.metrics.pending_samples = 352 * 51;
  }
  clock += 1000000000;
  WaitUntil(
      [&] { return service.Snapshot().health.level == "poor"; },
      "Observed pending degradation did not override missing clock evidence");
  WaitUntil([&] { return poor_health_logged.load(); },
            "Health diagnostic reasons must remain in logs when the standalone "
            "UI hint is removed");
  StatisticsViewModel model(service);
  model.Chart(false, 1, 600);
  model.Chart(true, 0, 3600);
  Require(service.SetAutoReconnect(true).get().status == CommandStatus::applied,
          "Serialize statistics preference writes");
  model.Refresh();
  auto chart_preferences = SettingsStore(root).Load().settings.statistics;
  Require(
      chart_preferences.primary == AppSettings::ChartPreference{1, 600} &&
          chart_preferences.secondary == AppSettings::ChartPreference{0, 3600},
      "Charts persist their metric and time range independently");
  StatisticsViewModel restored_charts(service);
  Require(restored_charts.ChartPreferences() == chart_preferences,
          "A recreated statistics view restores chart choices");
  Rejects([&] { model.Chart(false, 5, 600); },
          "Unknown chart metrics are rejected");
  Rejects([&] { model.Chart(true, 2, 120); },
          "Unknown chart time ranges are rejected");
  model.Refresh();
  model.Window(600);
  auto request = model.PrepareExport();
  StatisticsViewModel::StartExport(std::move(request),
                                   root / L"integrated.zip");
  WaitUntil(
      [&] {
        model.Refresh();
        return !model.ExportState().running;
      },
      "Background diagnostics never finished");
  Require(model.ExportState().result.status == CommandStatus::applied &&
              logs_flushed && std::filesystem::exists(root / L"integrated.zip"),
          "The statistics model exports actual public state asynchronously");
  {
    StatisticsViewModel canceled(service);
    auto canceled_request = canceled.PrepareExport();
    canceled.CancelExport();
    StatisticsViewModel::StartExport(std::move(canceled_request),
                                     root / L"picker-canceled.zip");
    canceled.Refresh();
    Require(canceled.ExportState().result.status == CommandStatus::canceled &&
                !std::filesystem::exists(root / L"picker-canceled.zip"),
            "Canceling during file selection remains canceled when a "
            "destination returns");
  }
  {
    auto view = std::make_unique<StatisticsViewModel>(service);
    auto lifecycle_request = view->PrepareExport();
    auto operation = lifecycle_request.operation;
    std::weak_ptr<ExportOperation> weak = operation;
    std::promise<void> started, release;
    auto released = release.get_future().share();
    operation->completion =
        std::async(std::launch::async, [weak, released, &started] {
          auto held = weak.lock();
          started.set_value();
          released.wait();
          CommandResult result;
          result.status =
              held->canceled ? CommandStatus::canceled : CommandStatus::applied;
          return result;
        });
    started.get_future().wait();
    auto destroyed =
        std::async(std::launch::async,
                   [view = std::move(view)]() mutable { view.reset(); });
    WaitUntil([&] { return operation->canceled.load(); },
              "Closing the statistics model must cancel its active export");
    bool waited = destroyed.wait_for(std::chrono::milliseconds(50)) !=
                  std::future_status::ready;
    release.set_value();
    destroyed.get();
    Require(waited &&
                operation->completion.wait_for(std::chrono::seconds(0)) ==
                    std::future_status::ready &&
                operation->completion.get().status == CommandStatus::canceled,
            "The model retains ownership and waits while an active worker "
            "holds the export operation");
  }
  fail_logs = true;
  Require(service.SetReceiverVolume(record.receiver_id, 0.4).get().status ==
              CommandStatus::applied,
          "A log sink failure does not turn a successful receiver command into "
          "a failure");
  WaitUntil(
      [&] { return !service.Snapshot().diagnostics_error.empty(); },
      "A log write failure must remain visible without breaking audio cleanup");
  fail_logs = false;
  {
    std::lock_guard lock(receiver.mutex);
    receiver.session_failure = std::pair(SENDER_NETWORK, 0);
  }
  WaitUntil([&] { return service.Snapshot().reconnect.has_value(); },
            "Prepare an observable recovery gap");
  before = service.Snapshot().metrics.sample_time;
  clock += 1000000000;
  WaitUntil([&] { return service.Snapshot().metrics.sample_time > before; },
            "Recovery did not produce a sampled history gap");
  Require(
      service.Snapshot().metrics.gap &&
          service.Snapshot().health.level == "poor" &&
          !service.Snapshot().metrics_history->gaps.empty(),
      "Recovery is recorded as a gap with its observed severe health state");
  Require(service.Disconnect().get().status == CommandStatus::applied,
          "Cancel receiver recovery after metrics collection");
  auto exit = service.Shutdown(true);
  Require(exit.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
              exit.get().status == CommandStatus::applied,
          "Metrics sampling does not extend bounded session shutdown");
}
