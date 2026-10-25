#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "../../../airplay2-sender-core/include/sender.h"
namespace Win2Air {
struct AudioSourceConfig {
  bool follow_system_default{true};
  std::string endpoint_id;
  std::string role{"multimedia"};
};
struct WindowPlacement {
  int x{}, y{}, width{1120}, height{780};
  bool saved{};
};
struct AppSettings {
  unsigned schema_version{1};
  uint64_t revision{};
  AudioSourceConfig audio_source;
  std::string selected_receiver_id;
  bool local_playback{true};
  bool auto_reconnect{true};
  bool receiver_media_control{true};
  // Empty retains the build's default until the user chooses a level.
  std::string log_level;
  std::string latency_mode{"balanced"};
  int custom_ms{1000}, silence_timeout{30};
  int standby_disconnect_seconds{};
  bool startup{}, minimized{}, restore_previous{}, close_to_tray{true};
  std::string theme{"system"}, language{"system"};
  WindowPlacement window_placement;
  struct ChartPreference {
    unsigned metric{}, seconds{60};
    bool operator==(ChartPreference const&) const = default;
  };
  struct StatisticsPreference {
    ChartPreference primary{3, 60}, secondary{2, 60};
    bool operator==(StatisticsPreference const&) const = default;
  } statistics;
};
struct ReceiverEndpoint {
  std::string host, resolved_address;
  unsigned port{7000}, interface_index{}, scope_id{};
  uint64_t discovery_epoch{}, last_seen{};
};
struct LatencyProfile {
  std::string firmware, network_context;
  int budget_ms{};
};
struct ReceiverRecord {
  std::string receiver_id, name, model, manufacturer;
  bool identity_verified{}, favorite{}, hidden{}, auto_connect{}, muted{},
      connect_established{};
  std::optional<ReceiverEndpoint> manual_endpoint;
  std::vector<ReceiverEndpoint> discovered_endpoints;
  std::optional<uint64_t> features;
  double volume{0.5};
  int offset_ms{};
  std::string firmware_version;
  std::optional<LatencyProfile> latency_profile;
};
struct SettingsSnapshot {
  AppSettings settings;
  std::vector<ReceiverRecord> receivers;
  bool first_run{};
};
struct CredentialRecord {
  unsigned schema_version{1};
  std::string receiver_id;
  uint64_t sender_id{}, updated_at{};
  std::vector<uint8_t> protected_blob;
};
bool ValidReceiverIdentity(std::string const& value) noexcept;
void ValidateSettings(SettingsSnapshot const& value);
}  // namespace Win2Air
