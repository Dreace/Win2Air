#include <cstdint>
#include "../../src/App/StorePreview.h"
#include "../../src/Metrics/MetricsHistory.h"
#include "../../src/Settings/Appearance.h"
#include "../../src/UI/ViewModels/ReceiverViewModel.h"
#include "../contract/test_support.h"
using namespace Win2Air;
void TestStorePreview() {
  auto chinese = StorePreview("zh-CN");
  auto english = StorePreview("en-US");
  Require(chinese.preferences.language == "zh-CN" &&
              chinese.preferences.theme == "light" &&
              chinese.preferences.latency_mode == "low" &&
              chinese.receivers.size() == 3 && chinese.sources.size() == 1 &&
              chinese.applied_volume == 42u && chinese.session &&
              chinese.session->intent.desired_connected &&
              chinese.session->session &&
              chinese.session->session->state == SessionState::streaming &&
              chinese.now_playing && chinese.now_playing->artwork.size() > 8 &&
              chinese.now_playing->artwork[0] == 0x89 &&
              chinese.metrics_history &&
              chinese.metrics_history->snapshots.size() == 61 &&
              chinese.health.level == "excellent",
          "Store preview shows a connected playback session");
  auto const& artwork = chinese.now_playing->artwork;
  auto quad = [](uint8_t const* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | bytes[3];
  };
  Require(artwork.size() > 24 && quad(artwork.data() + 16) == 64 &&
              quad(artwork.data() + 20) == 64,
          "Store preview artwork is a 64 pixel image");
  Require(chinese.now_playing->title == "\xe5\xa4\x9c\xe8\x88\xaa" &&
              chinese.receivers.front().name == "\xe5\xae\xa2\xe5\x8e\x85" &&
              english.now_playing->title == "Night Drive" &&
              english.receivers.front().name == "Living Room" &&
              english.preferences.language == "en-US",
          "Store preview uses the language of each store listing");
  resource_language = "zh-CN";
  Require(ReceiverDetail(chinese.receivers.front()) ==
              "HomePod mini\xef\xbc\x88\xe7\xac\xac\xe4\xb8\x80\xe4\xbb\xa3"
              "\xef\xbc\x89 | 192.168.1.21",
          "The Chinese store preview names the HomePod mini");
  resource_language = "en-US";
  Require(ReceiverDetail(english.receivers.front()) ==
              "HomePod mini (1st generation) | 192.168.1.21",
          "The English store preview names the HomePod mini");
  auto filled = [](PlaybackSnapshot const& preview, HistoryMetric metric) {
    int count = 0;
    for (auto const& bucket :
         AggregateHistory(*preview.metrics_history, preview.metrics.sample_time,
                          60, preview.metrics.receiver_id, metric))
      if (bucket.value) ++count;
    return count;
  };
  Require(filled(chinese, HistoryMetric::bitrate) == 60 &&
              filled(chinese, HistoryMetric::target_latency) == 60,
          "Store preview charts cover the one-minute window");
  Rejects([] { StorePreview("system"); },
          "Store preview accepts only the two listing languages");
}
