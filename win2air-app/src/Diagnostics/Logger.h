#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
namespace Win2Air {
enum class LogLevel { trace, debug, info, warning, error };
constexpr LogLevel DefaultLogLevel() noexcept {
#ifdef _DEBUG
  return LogLevel::debug;
#else
  return LogLevel::info;
#endif
}
LogLevel ResolveLogLevel(std::string_view value);
std::string_view LogLevelName(LogLevel value);
enum class LogModule {
  app,
  streaming,
  settings,
  credentials,
  audio,
  discovery
};
enum class LogEvent {
  command_submitted,
  command_applied,
  command_rejected,
  command_failed,
  command_canceled,
  persistence_failed,
  callback_failed,
  protocol_failed,
  protocol_state_observed,
  protocol_step_observed,
  protocol_control_observed,
  audio_timeline,
  session_changed,
  adaptive_latency,
  discovery_changed,
  media_control,
  settings_changed,
  recovery_changed,
  application_lifecycle
};
struct LogRecord {
  LogLevel level{LogLevel::info};
  LogModule module{LogModule::app};
  LogEvent event{LogEvent::command_applied};
  std::string receiver;
  unsigned state{};
  int code{}, platform_code{}, peer_code{};
  uint64_t generation{};
  std::string detail;
};
// Bounds and encodes exception text before it enters structured records.
void LogDiagnostic(std::function<void(LogRecord const&)> const& sink,
                   LogRecord record, std::string_view text);
class Logger {
  std::filesystem::path root_;
  std::mutex writer_;
  std::condition_variable wake_, drained_;
  std::deque<std::string> pending_;
  size_t pending_bytes_{};
  uint64_t dropped_{};
  bool stopping_{}, writing_{};
  std::exception_ptr failure_;
  std::thread worker_;
  std::function<std::chrono::steady_clock::time_point()> now_;
  std::chrono::steady_clock::time_point next_prune_;
  std::atomic<LogLevel> level_{DefaultLogLevel()};
  void Prune();
  static std::string Encode(LogRecord const& record);
  void Run() noexcept;
  void Persist(std::deque<std::string> const& lines);

 public:
  explicit Logger(std::filesystem::path root,
                  std::function<std::chrono::steady_clock::time_point()> now =
                      std::chrono::steady_clock::now);
  ~Logger();
  void Write(LogRecord const& record);
  // Used by export and tests; audio producers only enqueue through Write.
  void Flush();
  void SetLevel(LogLevel level);
  LogLevel Level() const noexcept {
    return level_.load(std::memory_order_relaxed);
  }
  bool Enabled(LogLevel level) const noexcept { return level >= Level(); }
};
}  // namespace Win2Air
