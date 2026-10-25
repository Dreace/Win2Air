#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace Win2Air {
enum class SystemEvent { suspend, resume, network_changed };
enum class CommandStatus { submitted, applied, rejected, failed, canceled };
enum class SessionState {
  disconnected,
  connecting,
  pairing,
  setting_up,
  ready,
  streaming,
  recovering,
  ending,
  error
};
enum class StandbyState { active, silence_pending, standby, resuming };
inline wchar_t const* ConnectedStatusKey(SessionState stage,
                                         StandbyState standby) {
  if (stage == SessionState::disconnected && standby == StandbyState::standby)
    return L"StatusWaitingForAudio";
  if (stage == SessionState::streaming && standby == StandbyState::standby)
    return L"StatusStandby";
  // Quiet PCM is not evidence that the source player has paused. Audio keeps
  // flowing until the configured standby timeout expires.
  if (stage == SessionState::streaming) return L"StatusPlaying";
  if (stage == SessionState::ready) return L"StatusStandby";
  return nullptr;
}
struct CommandResult {
  uint64_t command_id{}, intent_epoch{}, generation{};
  CommandStatus status{CommandStatus::submitted};
  SessionState stage{SessionState::disconnected};
  int platform_code{}, peer_code{};
  std::string message;
  int sender_code{};
};
struct ConnectionIntent {
  std::string selected_receiver_id;
  uint64_t intent_epoch{};
  bool desired_connected{}, suppressed_this_run{};
};
struct PresentationConfig {
  std::string mode{"balanced"};
  int base_ms{1000}, offset_ms{}, effective_ms{1000};
  uint64_t applied_samples{};
  CommandStatus status{CommandStatus::submitted};
  std::string reason;
};
struct ReceiverSession {
  std::string receiver_id;
  uint64_t generation{}, intent_epoch{};
  SessionState state{SessionState::disconnected};
  StandbyState standby_state{StandbyState::active};
  std::optional<CommandResult> error, last_command;
  PresentationConfig effective_presentation;
  std::string failure_kind;
};
struct SessionSnapshot {
  uint64_t revision{};
  ConnectionIntent intent;
  std::optional<ReceiverSession> session;
  CommandResult last_command;
};
using ImmutableSessionSnapshot = std::shared_ptr<SessionSnapshot const>;
struct ReconnectState {
  unsigned attempt{};
  uint64_t next_due_monotonic{}, cancellation_epoch{};
  std::string last_reason;
};
struct AudioFormat {
  unsigned rate{}, channels{}, bits{}, channel_mask{};
};
struct AudioSourceSnapshot {
  std::string endpoint_id, availability, error;
  AudioFormat mix_format;
  uint64_t source_epoch{};
};
struct CapturedBlock {
  uint64_t source_epoch{}, device_frame_position{}, capture_100ns{};
  unsigned frame_count{}, flags{};
  AudioFormat format;
  std::vector<uint8_t> owned_bytes;
};
struct ConvertedBlock {
  uint64_t source_epoch{}, timeline_epoch{}, first_sample{}, capture_100ns{};
  unsigned frame_count{};
  std::vector<uint8_t> pcm;
};
struct LocalPlaybackLease {
  std::string endpoint_id, app_context_guid;
  uint64_t lease_epoch{};
  double original_volume{};
  bool original_mute{}, owned_mute{}, external_change{}, restore_pending{};
};
enum class Availability { measured, estimated, unavailable };
struct MetricValue {
  std::optional<double> value;
  Availability availability{Availability::unavailable};
  std::string reason, source;
  uint64_t captured_at{};
};
struct MetricsSnapshot {
  std::string receiver_id;
  uint64_t generation{}, sample_time{}, uptime{};
  AudioFormat format;
  MetricValue bitrate, sent_packets, sent_bytes, resend_requests,
      resend_success, resend_expired;
  MetricValue pending, queue_fill, underruns, capture_discontinuities,
      target_latency, output_latency;
  MetricValue clock_lock, clock_offset, clock_drift, reconnect_count;
  std::optional<CommandResult> last_error;
  MetricValue captured_frames, submitted_frames, nonzero_frames;
  uint64_t app_captured_frames{}, app_submitted_frames{};
  bool recovering{}, recovered_event{}, gap{};
};
using ImmutableMetricsSnapshot = std::shared_ptr<MetricsSnapshot const>;
struct MetricsHistory {
  uint64_t generation{};
  std::vector<ImmutableMetricsSnapshot> snapshots;
  std::vector<uint64_t> gaps;
};
struct HealthSnapshot {
  std::string level{"unknown"};
  unsigned policy_version{1}, window{5};
  std::vector<std::string> reason_codes, inputs_available;
};
struct DiagnosticExport {
  std::string export_id, created_at, source_version;
  unsigned schema_version{1};
  CommandStatus result{CommandStatus::submitted};
  std::vector<std::string> safe_files;
};
}  // namespace Win2Air
