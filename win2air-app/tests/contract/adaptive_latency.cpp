#include <iostream>
#include <string_view>
#include <thread>
#include <vector>
#include "../../src/Audio/SpscRing.h"
#include "../../src/Streaming/AdaptiveLatencyController.h"
#include "../../src/Streaming/LatencyProfile.h"
namespace {
void Check(bool result, char const* message) {
  if (!result) throw std::runtime_error(message);
}
}  // namespace
void TestAdaptiveLatency() {
  using namespace Win2Air;
  {
    AppSettings settings;
    settings.latency_mode = "ultra";
    Check(InitialPresentation(settings, {}).base_ms == 80,
          "Ultra-low uses 80 ms");
    settings.latency_mode = "low";
    Check(InitialPresentation(settings, {}).base_ms == 120, "Low uses 120 ms");
    settings.latency_mode = "adaptive";
    auto pending = InitialPresentation(settings, {});
    sender_timing_info reported{};
    reported.receiver_render_available = 1;
    for (auto [value, expected] : {std::pair{0u, 30},
                                   {65u, 90},
                                   {66u, 100},
                                   {69u, 100},
                                   {70u, 100},
                                   {71u, 100},
                                   {80u, 110},
                                   {120u, 150},
                                   {4975u, 5000}}) {
      reported.receiver_render_ms = value;
      auto applied = ReceiverPresentation(pending, reported);
      Check(applied.base_ms == expected && applied.effective_ms == expected,
            "Receiver latency adds 25 ms before rounding upward");
    }
    for (auto value : {4976u, 5001u, UINT32_MAX}) {
      reported.receiver_render_ms = value;
      bool rejected = false;
      try {
        ReceiverPresentation(pending, reported);
      } catch (std::invalid_argument const&) {
        rejected = true;
      }
      Check(rejected, "Out-of-range receiver targets are rejected");
    }
    reported.receiver_render_available = 0;
    bool missing = false;
    try {
      ReceiverPresentation(pending, reported);
    } catch (std::invalid_argument const&) {
      missing = true;
    }
    Check(missing, "Missing receiver latency is distinct from zero");
    pending.offset_ms = 10;
    reported.receiver_render_available = 1;
    reported.receiver_render_ms = 69;
    Check(ReceiverPresentation(pending, reported).effective_ms == 110,
          "Internal compensation follows receiver rounding");
  }
  AdaptiveLatencyController controller;
  LatencyObservation sample;
  auto begin = [&](int budget, int offset = 0) {
    controller.Reset(budget, offset);
    sample = {1000000000ULL, 1, 100, 0, 0, 1, false};
    Check(!controller.Observe(sample), "The first sample establishes counters");
  };
  auto observe = [&](unsigned second, uint64_t resends = 0,
                     bool active = true) {
    sample.now_ns = uint64_t(second) * 1000000000ULL;
    if (active) sample.sent += 125;
    sample.retransmitted += resends;
    return controller.Observe(sample);
  };
  begin(90);
  Check(!observe(2, 1) && !observe(3, 1),
        "Isolated requests below the threshold keep the target");
  auto raise = observe(4, 1);
  Check(raise && raise->milliseconds == 100 &&
            std::string_view(raise->reason) == "resend",
        "Three requests within ten seconds raise the next 10 ms rung");
  controller.Confirm(raise->milliseconds, sample.now_ns);
  Check(!observe(5, 1) && !observe(6, 1),
        "A confirmed increase consumes its earlier request window");
  raise = observe(7, 1);
  Check(raise && raise->milliseconds == 110,
        "Persistent retransmissions continue increasing one rung at a time");
  controller.Confirm(raise->milliseconds, sample.now_ns);
  for (unsigned second = 8; second < 37; ++second)
    Check(!observe(second),
          "Recovery waits for thirty active seconds without retransmissions");
  auto lower = observe(37);
  Check(lower && lower->milliseconds == 100,
        "Stable transport automatically lowers by 10 ms");
  controller.Confirm(lower->milliseconds, sample.now_ns);
  Check(!observe(38, 1) && !observe(39, 1),
        "A single post-recovery request does not lock recovery");
  raise = observe(40, 1);
  Check(raise && raise->milliseconds == 110,
        "Repeated loss soon after recovery restores the higher rung");
  controller.Confirm(raise->milliseconds, sample.now_ns);
  Check(controller.RecoveryLocked(),
        "An unsuccessful recovery locks decreases for this connection");
  raise = observe(41, 3);
  Check(raise && raise->milliseconds == 120,
        "A recovery lock still permits further increases");
  controller.Confirm(raise->milliseconds, sample.now_ns);
  for (unsigned second = 42; second <= 180; ++second)
    Check(!observe(second),
          "A locked connection keeps its stable higher target");
  begin(90);
  Check(!controller.RecoveryLocked(),
        "A new connection clears the recovery lock");
  raise = observe(2, 20);
  Check(raise && raise->milliseconds == 100,
        "A burst raises one rung instead of jumping by its request count");
  controller.Confirm(raise->milliseconds, sample.now_ns);
  for (unsigned second = 3; second < 32; ++second)
    Check(!observe(second), "Stability accumulates continuously");
  lower = observe(32);
  Check(lower && lower->milliseconds == 90,
        "Recovery stops at the receiver-derived connection target");
  controller.Confirm(lower->milliseconds, sample.now_ns);
  for (unsigned second = 33; second <= 70; ++second)
    Check(!observe(second),
          "Healthy transport does not explore below the initial target");
  begin(90);
  Check(!observe(2, 1), "The first sparse request is recorded");
  for (unsigned second = 3; second <= 11; ++second)
    Check(!observe(second), "Sparse requests do not raise latency");
  Check(!observe(12, 1) && !observe(13, 1),
        "Requests at least ten seconds old leave the window");
  raise = observe(14, 1);
  Check(raise && raise->milliseconds == 100,
        "Only requests in the current rolling window contribute");
  controller.Confirm(raise->milliseconds, sample.now_ns);
  for (unsigned second = 15; second <= 35; ++second)
    Check(!observe(second), "A raised target remains during observation");
  Check(!observe(36, 1), "An isolated request restarts the stability timer");
  for (unsigned second = 37; second < 66; ++second)
    Check(!observe(second), "Stability excludes the latest isolated request");
  lower = observe(66);
  Check(lower && lower->milliseconds == 90,
        "Thirty seconds after the latest request permits recovery");
  controller.Confirm(lower->milliseconds, sample.now_ns);
  for (unsigned second = 67; second <= 77; ++second)
    Check(!observe(second),
          "The short relapse window can expire without locking");
  raise = observe(78, 3);
  controller.Confirm(raise->milliseconds, sample.now_ns);
  Check(!controller.RecoveryLocked(),
        "Later unrelated instability does not lock recovery");
  begin(90);
  raise = observe(2, 3);
  controller.Confirm(raise->milliseconds, sample.now_ns);
  for (unsigned second = 3; second <= 50; ++second)
    Check(!observe(second, 0, false),
          "Idle transport does not accumulate active stability");
  Check(!observe(51), "Resumed traffic starts a fresh stability interval");
  sample.now_ns = 100000000000ULL;
  sample.sent += 125;
  Check(!controller.Observe(sample),
        "Missing observations cannot prove continuous stability");
  begin(90);
  raise = observe(2, 3);
  controller.Reject(sample.now_ns);
  Check(controller.Applied() == 90 && !controller.RecoveryLocked(),
        "Rejected changes neither apply nor lock the connection");
  Check(!observe(3, 3), "Failed receiver changes observe their retry cooldown");
  begin(4890, 100);
  raise = observe(2, 3);
  Check(raise && raise->milliseconds == 4900,
        "The effective target respects the presentation ceiling");
  controller.Confirm(raise->milliseconds, sample.now_ns);
  Check(!observe(3, 3), "The ceiling prevents further increases");
  begin(90);
  sample.queue_age_ms = 200;
  Check(!observe(2),
        "A queue spike alone does not invent another increase rule");

  SpscRing<std::vector<uint64_t>, 32> queue;
  constexpr uint64_t frames = 100000;
  std::thread producer([&] {
    for (uint64_t index = 0; index < frames; ++index) {
      std::vector<uint64_t> packet{index, index ^ 0x55};
      while (!queue.Push(packet)) std::this_thread::yield();
    }
  });
  bool ordered = true;
  for (uint64_t index = 0; index < frames; ++index) {
    std::optional<std::vector<uint64_t>> packet;
    while (!(packet = queue.Pop())) std::this_thread::yield();
    ordered &= packet->size() == 2 && packet->front() == index &&
               packet->back() == (index ^ 0x55);
  }
  producer.join();
  Check(ordered && !queue.Pop(),
        "Concurrent capture transfer preserves order and ownership");
  ReceiverRecord receiver;
  receiver.identity_verified = true;
  receiver.firmware_version = "virtual-firmware";
  ReceiverEndpoint endpoint{"virtual", "127.0.0.1", 7000, 1};
  receiver.latency_profile = LatencyProfile{
      receiver.firmware_version, LatencyNetworkContext(endpoint), 20};
  Check(SavedLatency(receiver, endpoint) == 20,
        "A matching verified device and route restore its profile");
  endpoint.interface_index = 2;
  Check(!SavedLatency(receiver, endpoint),
        "A route change invalidates the profile");
  endpoint.interface_index = 1;
  receiver.firmware_version = "changed-firmware";
  Check(!SavedLatency(receiver, endpoint),
        "A firmware change invalidates the profile");
  ReceiverEndpoint home{"virtual", "192.168.1.117", 7000, 7};
  ReceiverRecord homepod;
  homepod.model = "AudioAccessory5,1";
  homepod.latency_profile = LatencyProfile{"model:AudioAccessory5,1",
                                           LatencyNetworkContext(home), 90};
  Check(SavedLatency(homepod, home) == 90,
        "Model identity supports profiles without firmware metadata");
  homepod.latency_profile->budget_ms = 73;
  AppSettings settings;
  settings.latency_mode = "balanced";
  homepod.receiver_id = "06:9F:C6:B5:52:C8";
  settings.selected_receiver_id = homepod.receiver_id;
  homepod.manual_endpoint = home;
  Check(!RecentAdaptiveBudget(settings, {homepod}, {}),
        "Historical targets never replace receiver timing");
  homepod.latency_profile->budget_ms = 90;
  home.interface_index = 8;
  Check(!SavedLatency(homepod, home),
        "A different route does not reuse a saved target");
  home.interface_index = 7;
  homepod.manual_endpoint = home;
  Check(!RecentAdaptiveBudget(settings, {homepod}, 1000),
        "Another mode keeps adaptive timing pending");
  settings.latency_mode = "adaptive";
  Check(!RecentAdaptiveBudget(settings, {homepod}, {}),
        "Receiver timing remains pending before setup");
  Check(RecentAdaptiveBudget(settings, {homepod}, 86) == 86,
        "An active session displays its current delay");
  settings.selected_receiver_id.clear();
  Check(!RecentAdaptiveBudget(settings, {homepod}, {}),
        "The first session has no invented timing");
}
#ifdef WIN2AIR_STANDALONE_TEST
int main() {
  try {
    TestAdaptiveLatency();
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
#endif
