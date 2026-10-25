#pragma once
#include <string>
#include <vector>
#include "../App/SystemMediaSession.h"
#include "../Audio/FormatConverter.h"
#include "../Audio/LocalPlaybackController.h"
#include "../Audio/RingBuffer.h"
#include "../Audio/SilenceDetector.h"
#include "../Audio/WasapiCapture.h"
#include "../Diagnostics/Logger.h"
#include "../Discovery/ReceiverRegistry.h"
#include "../Discovery/ReceiverResolver.h"
#include "../Metrics/HealthEvaluator.h"
#include "../Metrics/MetricsCollector.h"
#include "../Metrics/MetricsHistory.h"
#include "../Settings/SettingsStore.h"
#include "AdaptiveLatencyController.h"
#include "CommandDispatcher.h"
#include "FailureDetail.h"
#include "LatencyController.h"
#include "PresentationConfig.h"
#include "ReconnectController.h"
#include "VolumeController.h"
namespace Win2Air {
struct StreamNotice {
  uint64_t id{};
  std::string code;
  int latency_ms{};
  std::string receiver_id, detail;
  std::chrono::system_clock::time_point occurred_at;
};
struct PlaybackSnapshot {
  AppSettings preferences;
  ImmutableSessionSnapshot session;
  std::vector<AudioDevice> sources;
  std::vector<ReceiverRecord> receivers;
  AudioSourceSnapshot source;
  AudioSourceConfig source_config;
  bool discovering{}, discovery_refreshing{}, local_playback{true};
  std::optional<unsigned> applied_volume;
  bool volume_pending{};
  bool presentation_pending{};
  uint64_t submitted_frames{}, captured_frames{}, discontinuities{};
  uint64_t nonzero_frames{};
  uint64_t dropped_capture_blocks{};
  uint64_t future_hold_max_ns{};
  std::map<std::string, uint64_t> discontinuity_reasons;
  sender_timing_info timing{};
  CapturePeriod capture_period;
  std::string discovery_error, local_error, credential_error;
  std::optional<ReconnectState> reconnect;
  uint64_t recovery_now{};
  MetricsSnapshot metrics;
  HealthSnapshot health;
  std::shared_ptr<MetricsHistory const> metrics_history;
  std::string metrics_error;
  std::string adaptive_error;
  std::string diagnostics_error;
  std::vector<StreamNotice> notices;
  std::shared_ptr<NowPlaying const> now_playing;
  std::string metadata_error;
};
struct AudioServices {
  std::function<bool(uint32_t)> media_control;
  std::function<std::shared_ptr<NowPlaying const>()> now_playing;
  std::function<std::unique_ptr<DeviceManager>()> devices;
  std::function<std::unique_ptr<WasapiCapture>()> capture;
  std::function<std::unique_ptr<LocalPlaybackController>(
      std::filesystem::path const&)>
      local_playback;
  std::function<uint64_t()> silence_now;
  std::function<uint64_t()> recovery_now;
  std::function<uint64_t()> metrics_now;
  std::function<uint64_t()> audio_now;
  std::function<void(LogRecord const&)> log_sink;
  std::function<void()> log_flush;
  FormatConverter::TransformFactory transform;
  std::function<void(LogLevel)> log_level_changed;
};
class StreamCoordinator {
  AudioServices audio_services_;
  std::unique_ptr<SystemMediaSession> system_media_;
  std::string logged_metadata_error_;
  uint64_t media_generation_{}, media_revision_{}, volume_observed_revision_{};
  std::string metadata_transport_error_;
  SenderBackend& library_;
  SettingsStore store_;
  CredentialStore credentials_;
  CredentialStore authentication_inputs_;
  std::filesystem::path root_;
  SettingsSnapshot settings_;
  ReceiverRegistry registry_;
  sender_runtime* runtime_{};
  uint64_t sender_id_{}, generation_{}, source_epoch_{}, timeline_{},
      revision_{}, next_command_{};
  std::atomic<uint64_t> intent_epoch_{1};
  uint64_t active_intent_{};
  std::atomic<bool> stopping_{};
  std::thread worker_;
  std::atomic<bool> shutting_down_{};
  std::atomic<BrowseDeadline> shutdown_deadline_{BrowseDeadline::max()};
  std::mutex shutdown_mutex_;
  std::shared_future<CommandResult> shutdown_result_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::atomic<bool> capture_ready_{};
  std::deque<std::function<void()>> commands_;
  PlaybackSnapshot snapshot_;
  SessionSnapshot session_;
  std::unique_ptr<DeviceManager> devices_;
  std::unique_ptr<WasapiCapture> capture_;
  std::unique_ptr<FormatConverter> converter_;
  std::unique_ptr<LocalPlaybackController> local_;
  AudioRingBuffer audio_{44100};
  std::deque<ConvertedBlock> converted_;
  size_t conversion_offset_{}, converted_bytes_{};
  uint32_t sample_shift_{};
  int64_t capture_rebase_100ns_{};
  std::unique_ptr<DnsSdBrowser> browser_;
  std::future<void> discovery_refresh_;
  uint64_t discovery_epoch_{1};
  std::future<ResolvedReceiver> discovery_resolution_;
  std::string resolving_instance_;
  uint64_t resolving_epoch_{};
  bool resolving_removed_{};
  std::map<std::string, ServiceAnnouncement> announcements_;
  std::map<std::string, std::pair<std::string, ReceiverEndpoint>>
      discovered_services_;
  std::string endpoint_, selected_;
  std::string logged_session_id_;
  uint64_t logged_session_generation_{};
  SessionState logged_session_state_{SessionState::disconnected};
  StandbyState logged_standby_{StandbyState::active};
  std::vector<uint8_t> mix_;
  std::optional<SecureBuffer> returned_credentials_;
  SecureBuffer pending_authentication_input_;
  bool authentication_input_used_{};
  bool callback_failed_{}, capturing_{};
  bool exit_cleanup_failed_{};
  std::shared_ptr<std::promise<CommandResult>> connecting_;
  bool discovery_enabled_{};
  CommandResult connecting_result_;
  uint64_t volume_sequence_{};
  SilenceDetector silence_;
  std::optional<uint64_t> standby_since_;
  bool idle_disconnected_{};
  std::optional<sender_command_completion> presentation_completion_;
  uint64_t awaited_presentation_{};
  std::optional<sender_command_completion> volume_completion_;
  uint64_t awaited_volume_{};
  ReconnectController reconnect_;
  bool desired_connected_{}, suppressed_{}, suspended_{}, reconnecting_{};
  StandbyState recovery_standby_{StandbyState::active};
  std::atomic<unsigned> system_events_{};
  std::atomic<unsigned> changed_interface_{};
  std::chrono::steady_clock::time_point connection_deadline_;
  MetricsCollector metrics_;
  MetricsHistoryBuffer history_;
  HealthEvaluator health_;
  AdaptiveLatencyController adaptive_;
  uint64_t adaptive_generation_{}, submit_age_ns_{}, adaptive_dropped_{};
  uint64_t anchored_discontinuities_{};
  uint64_t adaptive_failed_command_{}, notice_id_{};
  uint64_t last_metrics_{}, recoveries_{};
  bool was_connected_{};
  std::deque<sender_log_record> protocol_logs_;
  bool log_failed_{};
  static void SENDER_CALL SafeLog(void*, sender_log_record const*) noexcept;
  void SampleMetrics();
  void AdaptLatency(uint64_t now, std::optional<sender_snapshot> const&,
                    CaptureCounters const&, sender_timing_info,
                    uint64_t dropped, uint64_t queue_age_ns);
  void DrainLogs();
  static void SENDER_CALL
  VolumeCompleted(void*, sender_command_completion const*) noexcept;
  static void SENDER_CALL State(void*, uint64_t, sender_state,
                                sender_error const*) noexcept;
  static void SENDER_CALL Step(void*, uint64_t, sender_step) noexcept;
  static void SENDER_CALL Credentials(void*, uint64_t,
                                      sender_credentials const*) noexcept;
  void Run(std::promise<void> started);
  void Publish();
  void Fail(std::string message, int platform = 0, int peer = 0,
            int code = SENDER_PLATFORM);
  void Create(PresentationConfig const&);
  void Cleanup(bool restore_local = true, bool retain_session = false);
  void CloseReceiver();
  void PumpIdleSource();
  void RevokePairing(std::string const& identity);
  void BeginConnection(CommandResult, SecureBuffer const&,
                       std::shared_ptr<std::promise<CommandResult>> = {});
  void Recover(FailureDetail const&);
  void TickRecovery();
  void AutoConnectSelected();
  void SystemEvents();
  uint64_t RecoveryNow() const;
  std::string ResolveSource();
  void StartSource();
  void FlushSource(std::string reason = "source-change",
                   std::chrono::milliseconds timeout = std::chrono::seconds(3));
  void ParkSource(std::string reason);
  void NoteAudio(std::string detail);
  void Note(LogLevel, LogModule, LogEvent, std::string detail, int code = 0,
            int platform = 0, int peer = 0);
  void NoteStream(std::string code, int latency_ms);
  void AddNotice(std::string code, std::string detail = {}, int latency_ms = 0);
  void RecordConnection(bool established);
  CommandResult ReopenStream(CommandResult result);
  bool MovePresentation(PresentationConfig const& next);
  void Pump();
  void DrainAudio();
  void ClearAudio();
  bool QueueAudio(ConvertedBlock block);
  void Discover();
  void StopDiscovery(
      BrowseDeadline deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2));
  void Save();
  bool ApplyVolume(unsigned percent, uint64_t epoch);
  uint64_t SilenceNow() const;
  bool ApplyPresentation(PresentationConfig const&, uint64_t epoch);
  CommandResult ChangePresentation(CommandResult, AppSettings, ReceiverRecord);
  CommandResult ChangeVolume(CommandResult result, ReceiverRecord record);
  std::future<CommandResult> Queue(
      bool supersede, std::function<CommandResult(CommandResult)> action,
      bool shutdown = false);

 public:
  StreamCoordinator(SenderBackend&, std::filesystem::path root,
                    bool discover = true, AudioServices audio_services = {},
                    std::unique_ptr<DnsSdBrowser> browser = {});
  ~StreamCoordinator();
  PlaybackSnapshot Snapshot();
  std::filesystem::path DataRoot() const { return root_; }
  std::future<CommandResult> Connect(std::string identity,
                                     SecureBuffer pin = {});
  std::future<CommandResult> Disconnect();
  std::future<CommandResult> RefreshReceivers();
  std::future<CommandResult> SetStatistics(
      AppSettings::StatisticsPreference value);
  std::future<CommandResult> SetAutoReconnect(bool enabled);
  void NotifySystemEvent(SystemEvent, unsigned interface_index = 0) noexcept;
  std::shared_future<CommandResult> Shutdown(
      bool final_exit = false, std::optional<WindowPlacement> placement = {});
  std::future<CommandResult> SetLifecycle(bool close_to_tray, bool minimized,
                                          bool restore_previous);
  std::future<CommandResult> SetStartup(bool enabled);
  std::future<CommandResult> SaveWindow(WindowPlacement placement);
  std::future<CommandResult> SetSource(AudioSourceConfig source);
  std::future<CommandResult> SetLocalPlayback(bool enabled);
  std::future<CommandResult> SetReceiverMediaControl(bool enabled);
  std::future<CommandResult> SetLogLevel(std::string level);
  std::function<void()> LogFlusher() const { return audio_services_.log_flush; }
  std::future<CommandResult> AddReceiver(std::string host, unsigned port,
                                         int initial_offset = 0);
  std::future<CommandResult> SetReceiverVolume(std::string identity,
                                               double value);
  std::future<CommandResult> SetReceiverMuted(std::string identity, bool muted);
  std::future<CommandResult> SetLatency(std::string mode, int custom_ms);
  std::future<CommandResult> SetOffset(std::string identity, int offset_ms);
  std::future<CommandResult> SetSilenceTimeout(int seconds);
  std::future<CommandResult> SetStandbyDisconnect(int seconds);
  std::future<CommandResult> UpdateReceiver(std::string identity,
                                            std::string name, bool favorite,
                                            bool hidden, bool auto_connect,
                                            int offset_ms);
  std::future<CommandResult> EditReceiverEndpoint(std::string identity,
                                                  std::string host,
                                                  unsigned port);
  std::future<CommandResult> DeleteReceiver(std::string identity);
  std::future<CommandResult> ForgetPairing(std::string identity);
  std::future<CommandResult> ResetAdaptiveLatency(std::string identity);
  std::future<CommandResult> SetAppearance(std::string theme,
                                           std::string language);
};
}  // namespace Win2Air
