#include "StorePreview.h"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace Win2Air {
namespace {
constexpr char const* kLivingRoom = "AA:BB:CC:DD:EE:01";
constexpr uint64_t kSecond = 1000000000ULL;

MetricValue Measured(double value) {
  MetricValue metric;
  metric.value = value;
  metric.availability = Availability::measured;
  return metric;
}
uint32_t Crc32(uint8_t const* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}
uint32_t Adler32(uint8_t const* data, size_t size) {
  uint32_t a = 1, b = 0;
  for (size_t index = 0; index < size; ++index) {
    a = (a + data[index]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}
void Append32(std::vector<uint8_t>& bytes, uint32_t value) {
  bytes.push_back(uint8_t(value >> 24));
  bytes.push_back(uint8_t(value >> 16));
  bytes.push_back(uint8_t(value >> 8));
  bytes.push_back(uint8_t(value));
}
void AppendChunk(std::vector<uint8_t>& png, char const* type,
                 std::vector<uint8_t> const& data) {
  Append32(png, uint32_t(data.size()));
  auto start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  Append32(png, Crc32(png.data() + start, png.size() - start));
}
std::vector<uint8_t> Artwork() {
  constexpr int size = 64;
  std::vector<uint8_t> pixels;
  pixels.reserve(size_t(size) * (size * 3 + 1));
  for (int y = 0; y < size; ++y) {
    pixels.push_back(0);
    for (int x = 0; x < size; ++x) {
      pixels.push_back(uint8_t(32 + x * 2));
      pixels.push_back(uint8_t(64 + y * 2));
      pixels.push_back(uint8_t(180 - x));
    }
  }
  std::vector<uint8_t> deflate;
  deflate.push_back(0x78);
  deflate.push_back(0x01);
  deflate.push_back(0x01);
  auto length = uint16_t(pixels.size());
  deflate.push_back(uint8_t(length));
  deflate.push_back(uint8_t(length >> 8));
  auto complement = uint16_t(~length);
  deflate.push_back(uint8_t(complement));
  deflate.push_back(uint8_t(complement >> 8));
  deflate.insert(deflate.end(), pixels.begin(), pixels.end());
  Append32(deflate, Adler32(pixels.data(), pixels.size()));
  std::vector<uint8_t> header;
  Append32(header, uint32_t(size));
  Append32(header, uint32_t(size));
  header.push_back(8);
  header.push_back(2);
  header.push_back(0);
  header.push_back(0);
  header.push_back(0);
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  AppendChunk(png, "IHDR", header);
  AppendChunk(png, "IDAT", deflate);
  AppendChunk(png, "IEND", {});
  return png;
}
ReceiverRecord Device(std::string id, std::string name, std::string model,
                      std::string address, bool favorite) {
  ReceiverRecord record;
  record.receiver_id = std::move(id);
  record.name = std::move(name);
  record.model = std::move(model);
  record.manufacturer = "Apple";
  record.favorite = favorite;
  record.identity_verified = true;
  record.volume = favorite ? 0.42 : 0.35;
  record.manual_endpoint = ReceiverEndpoint{address, address, 7000};
  return record;
}
}  // namespace
PlaybackSnapshot StorePreview(std::string const& language) {
  bool chinese = language == "zh-CN";
  if (!chinese && language != "en-US")
    throw std::invalid_argument(
        "Store preview language must be zh-CN or en-US");
  PlaybackSnapshot snapshot;
  snapshot.preferences.language = language;
  snapshot.preferences.theme = "light";
  snapshot.preferences.latency_mode = "low";
  snapshot.preferences.custom_ms = 1000;
  snapshot.preferences.silence_timeout = 30;
  snapshot.preferences.log_level = "info";
  snapshot.preferences.local_playback = true;
  snapshot.preferences.auto_reconnect = true;
  snapshot.preferences.receiver_media_control = true;
  snapshot.preferences.close_to_tray = true;
  snapshot.preferences.restore_previous = true;
  snapshot.preferences.selected_receiver_id = kLivingRoom;
  snapshot.preferences.statistics.primary = {3, 60};
  snapshot.preferences.statistics.secondary = {2, 60};
  snapshot.receivers = {
      Device(kLivingRoom, chinese ? "\xe5\xae\xa2\xe5\x8e\x85" : "Living Room",
             "AudioAccessory5,1", "192.168.1.21", true),
      Device("AA:BB:CC:DD:EE:02",
             chinese ? "\xe4\xb9\xa6\xe6\x88\xbf" : "Study",
             "AudioAccessory6,1", "192.168.1.22", false),
      Device("AA:BB:CC:DD:EE:03",
             chinese ? "\xe5\x8d\xa7\xe5\xae\xa4" : "Bedroom", "AppleTV14,1",
             "192.168.1.23", false)};
  AudioDevice speakers;
  speakers.id = "{0.0.0.00000000}.{store-speakers}";
  speakers.name = chinese ? "\xe6\x89\xac\xe5\xa3\xb0\xe5\x99\xa8" : "Speakers";
  speakers.is_default = true;
  snapshot.sources = {speakers};
  snapshot.preferences.audio_source.follow_system_default = false;
  snapshot.preferences.audio_source.endpoint_id = speakers.id;
  snapshot.source_config = snapshot.preferences.audio_source;
  snapshot.local_playback = true;
  snapshot.applied_volume = 42;
  auto session = std::make_shared<SessionSnapshot>();
  session->intent.selected_receiver_id = kLivingRoom;
  session->intent.desired_connected = true;
  ReceiverSession active;
  active.receiver_id = kLivingRoom;
  active.generation = 1;
  active.state = SessionState::streaming;
  active.effective_presentation.mode = "low";
  active.effective_presentation.base_ms = 120;
  active.effective_presentation.effective_ms = 120;
  active.effective_presentation.status = CommandStatus::applied;
  session->session = active;
  session->last_command.status = CommandStatus::applied;
  snapshot.session = session;
  constexpr int samples = 61;
  uint64_t now = uint64_t(samples) * kSecond;
  auto history = std::make_shared<MetricsHistory>();
  history->generation = 1;
  for (int index = 0; index < samples; ++index) {
    MetricsSnapshot sample;
    sample.receiver_id = kLivingRoom;
    sample.generation = 1;
    sample.sample_time = now - uint64_t(samples - 1 - index) * kSecond;
    sample.bitrate = Measured(1000000.0 + 250000.0 * std::sin(index / 6.0));
    sample.target_latency = Measured(120);
    sample.sent_bytes = Measured(1600000.0 * index);
    sample.sent_packets = Measured(140.0 * index);
    sample.resend_requests = Measured(index / 20.0);
    history->snapshots.push_back(
        std::make_shared<MetricsSnapshot const>(std::move(sample)));
  }
  snapshot.metrics = *history->snapshots.back();
  snapshot.metrics.uptime = (12 * 60 + 24) * kSecond;
  snapshot.metrics.format = {44100, 2, 16, 0};
  snapshot.metrics.pending = Measured(2);
  snapshot.metrics.sent_bytes = Measured(96e6);
  snapshot.metrics.sent_packets = Measured(93000);
  snapshot.metrics.resend_requests = Measured(18);
  snapshot.metrics.resend_success = Measured(18);
  snapshot.metrics.captured_frames = Measured(44100.0 * 744);
  snapshot.metrics.submitted_frames = Measured(44100.0 * 744);
  snapshot.metrics.nonzero_frames = Measured(44100.0 * 700);
  snapshot.metrics_history = history;
  snapshot.health.level = "excellent";
  auto playing = std::make_shared<NowPlaying>();
  playing->title = chinese ? "\xe5\xa4\x9c\xe8\x88\xaa" : "Night Drive";
  playing->artist = chinese ? "\xe6\x9e\x97\xe5\xa4\x8f" : "Lena Hart";
  playing->album = chinese ? "\xe7\x81\xaf\xe7\x81\xab" : "After Hours";
  playing->artwork_type = "image/png";
  playing->artwork = Artwork();
  playing->revision = 1;
  snapshot.now_playing = playing;
  auto occurred = std::chrono::system_clock::now() - std::chrono::minutes(2);
  snapshot.notices.push_back(
      {1, "content", 0, kLivingRoom, playing->title, occurred});
  snapshot.notices.push_back(
      {2, "latency", 120, kLivingRoom, {}, occurred + std::chrono::minutes(1)});
  snapshot.timing.size = sizeof(snapshot.timing);
  snapshot.timing.version = 1;
  snapshot.timing.base_ms = 120;
  snapshot.timing.effective_ms = 120;
  snapshot.timing.setup_applied = 1;
  return snapshot;
}
}  // namespace Win2Air
