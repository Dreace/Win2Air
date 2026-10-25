#include "Logger.h"

#include <chrono>
#include <fstream>
#include <utility>

#include "../Settings/AppSettings.h"
#include "../Settings/StorageIO.h"
namespace Win2Air {
void LogDiagnostic(std::function<void(LogRecord const&)> const& sink,
                   LogRecord record, std::string_view text) {
  if (!sink) return;
  bool truncated = text.size() > 4096;
  text = text.substr(0, 4096);
  constexpr char hex[] = "0123456789abcdef";
  record.detail.clear();
  for (unsigned char c : text) {
    if (record.detail.size() > 232) {
      sink(record);
      record.detail.clear();
    }
    if (c >= 32 && c < 127)
      record.detail += char(c);
    else {
      record.detail += '[';
      record.detail += hex[c >> 4];
      record.detail += hex[c & 15];
      record.detail += ']';
    }
  }
  if (!record.detail.empty() || text.empty()) sink(record);
  if (truncated) {
    record.detail = "diagnostic text truncated after 4096 bytes";
    sink(record);
  }
}
std::string_view LogLevelName(LogLevel value) {
  constexpr std::string_view names[]{"trace", "debug", "info", "warning",
                                     "error"};
  if (unsigned(value) >= std::size(names))
    throw std::invalid_argument("Unknown log level");
  return names[unsigned(value)];
}
LogLevel ResolveLogLevel(std::string_view value) {
  if (value.empty()) return DefaultLogLevel();
  for (auto level : {LogLevel::trace, LogLevel::debug, LogLevel::info,
                     LogLevel::warning, LogLevel::error})
    if (LogLevelName(level) == value) return level;
  throw std::invalid_argument("Unknown log level");
}
void Logger::SetLevel(LogLevel level) {
  LogLevelName(level);
  level_.store(level, std::memory_order_relaxed);
}
Logger::Logger(std::filesystem::path root,
               std::function<std::chrono::steady_clock::time_point()> now)
    : root_(std::move(root)), now_(std::move(now)) {
  if (!now_) throw std::invalid_argument("Log retention clock is required");
  StorageIO::PrivateDirectory(root_);
  Prune();
  next_prune_ = now_() + std::chrono::minutes(1);
  worker_ = std::thread([this] { Run(); });
}
Logger::~Logger() {
  {
    std::lock_guard lock(writer_);
    stopping_ = true;
  }
  wake_.notify_one();
  if (worker_.joinable()) worker_.join();
  if (failure_) {
    try {
      std::rethrow_exception(failure_);
    } catch (std::exception const& error) {
      OutputDebugStringA(error.what());
    } catch (...) {
      OutputDebugStringA("Win2Air asynchronous log writer failed");
    }
  }
}
void Logger::Prune() {
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
  for (unsigned i = 0; i < 5; ++i) {
    auto path = root_ / (i ? "app." + std::to_string(i) + ".log" : "app.log");
    if (std::filesystem::exists(path)) {
      std::ifstream input(path);
      std::string first;
      std::getline(input, first);
      if (!first.starts_with("{\"timestamp\":"))
        throw std::runtime_error("Log file contains unknown data");
      auto end = first.find(',', 13);
      auto text = first.substr(13, end - 13);
      size_t used{};
      auto stamp = std::stoll(text, &used);
      if (used != text.size() || stamp < 0 || stamp > now)
        throw std::runtime_error("Log timestamp invalid");
      input.close();
      if (now - stamp > 7LL * 24 * 60 * 60 * 1000)
        std::filesystem::remove(path);
    }
  }
}
std::string Logger::Encode(LogRecord const& record) {
  static char const* levels[] = {"Trace", "Debug", "Info", "Warning", "Error"};
  static char const* modules[] = {"App",         "Streaming", "Settings",
                                  "Credentials", "Audio",     "Discovery"};
  static char const* events[] = {
      "command-submitted",         "command-applied",
      "command-rejected",          "command-failed",
      "command-canceled",          "persistence-failed",
      "callback-failed",           "protocol-failed",
      "protocol-state-observed",   "protocol-step-observed",
      "protocol-control-observed", "audio-timeline",
      "session-changed",           "adaptive-latency",
      "discovery-changed",         "media-control",
      "settings-changed",          "recovery-changed",
      "application-lifecycle"};
  static char const* messages[] = {"Command queued",
                                   "Command completed",
                                   "Command rejected",
                                   "Command failed",
                                   "Command canceled",
                                   "Persistence failed",
                                   "Callback copy failed",
                                   "Protocol operation failed",
                                   "Protocol state observed",
                                   "Protocol step observed",
                                   "Protocol control observed",
                                   "Audio timeline observed",
                                   "Session state changed",
                                   "Adaptive latency changed",
                                   "Receiver discovery changed",
                                   "Receiver media control",
                                   "Settings changed",
                                   "Connection recovery changed",
                                   "Application lifecycle"};
  auto level = unsigned(record.level), module = unsigned(record.module),
       event = unsigned(record.event);
  if (level >= std::size(levels) || module >= std::size(modules) ||
      event >= std::size(events) ||
      (!record.receiver.empty() && !ValidReceiverIdentity(record.receiver)))
    throw std::runtime_error(
        "Log fields must contain safe known events and public identity");
  if (record.detail.size() > 240)
    throw std::runtime_error("Audio log detail exceeds 240 characters");
  for (unsigned char character : record.detail)
    if (character < 0x20 || character > 0x7e)
      throw std::runtime_error("Audio log detail must be printable ASCII");
  std::string detail;
  for (char character : record.detail) {
    if (character == '"' || character == '\\') detail += '\\';
    detail += character;
  }
  auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  std::string line =
      "{\"timestamp\":" + std::to_string(stamp) +
      ",\"thread\":" + std::to_string(GetCurrentThreadId()) + ",\"level\":\"" +
      levels[level] + "\",\"module\":\"" + modules[module] +
      "\",\"receiver\":\"" + record.receiver +
      "\",\"state\":" + std::to_string(record.state) +
      ",\"session\":" + std::to_string(record.generation) + ",\"event\":\"" +
      events[event] + "\",\"code\":" + std::to_string(record.code) +
      ",\"platform_code\":" + std::to_string(record.platform_code) +
      ",\"peer_code\":" + std::to_string(record.peer_code) + ",\"message\":\"" +
      messages[event] + "\"" +
      (detail.empty() ? "" : ",\"detail\":\"" + detail + "\"") + "}\n";
  return line;
}
void Logger::Write(LogRecord const& record) {
  auto line = Encode(record);
  if (!Enabled(record.level)) return;
  {
    std::lock_guard lock(writer_);
    if (failure_) std::rethrow_exception(failure_);
    if (stopping_) throw std::runtime_error("Log writer is stopping");
    // Bound both queue nodes and payloads when storage cannot keep up.
    if (pending_.size() >= 4096 ||
        pending_bytes_ + line.size() > 4 * 1024 * 1024) {
      ++dropped_;
    } else {
      pending_.push_back(std::move(line));
      pending_bytes_ += pending_.back().size();
    }
  }
  wake_.notify_one();
}
void Logger::Flush() {
  std::unique_lock lock(writer_);
  drained_.wait(lock, [this] {
    return failure_ || (pending_.empty() && !writing_ && !dropped_);
  });
  if (failure_) std::rethrow_exception(failure_);
}
void Logger::Run() noexcept {
  try {
    for (;;) {
      std::deque<std::string> lines;
      uint64_t dropped;
      {
        std::unique_lock lock(writer_);
        wake_.wait(lock, [this] {
          return stopping_ || !pending_.empty() || dropped_;
        });
        if (pending_.empty() && !dropped_ && stopping_) return;
        lines.swap(pending_);
        pending_bytes_ = 0;
        dropped = std::exchange(dropped_, uint64_t{0});
        writing_ = true;
      }
      if (dropped)
        lines.push_back(
            Encode({LogLevel::warning, LogModule::app,
                    LogEvent::application_lifecycle, "", 0, 0, 0, 0, 0,
                    "logger queue_overflow dropped_records=" +
                        std::to_string(dropped)}));
      Persist(lines);
      {
        std::lock_guard lock(writer_);
        writing_ = false;
      }
      drained_.notify_all();
    }
  } catch (...) {
    {
      std::lock_guard lock(writer_);
      failure_ = std::current_exception();
      pending_.clear();
      pending_bytes_ = 0;
      writing_ = false;
    }
    drained_.notify_all();
  }
}
void Logger::Persist(std::deque<std::string> const& lines) {
  if (auto now = now_(); now >= next_prune_) {
    Prune();
    next_prune_ = now + std::chrono::minutes(1);
  }
  auto path = root_ / L"app.log";
  auto bytes =
      std::filesystem::exists(path) ? std::filesystem::file_size(path) : 0;
  std::ofstream stream;
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  for (auto const& line : lines) {
    if (bytes + line.size() > 10 * 1024 * 1024) {
      if (stream.is_open()) stream.close();
      std::filesystem::remove(root_ / L"app.4.log");
      for (int i = 3; i >= 0; --i) {
        auto from =
            root_ / (i ? "app." + std::to_string(i) + ".log" : "app.log");
        auto to = root_ / ("app." + std::to_string(i + 1) + ".log");
        if (std::filesystem::exists(from)) std::filesystem::rename(from, to);
      }
      bytes = 0;
    }
    if (!stream.is_open()) stream.open(path, std::ios::binary | std::ios::app);
    stream << line;
    bytes += line.size();
  }
  stream.flush();
}
}  // namespace Win2Air
