#include <winrt/Windows.Data.Json.h>
#include <fstream>
#include <future>
#include <iterator>
#include <vector>

#include "../../src/Diagnostics/Logger.h"
#include "test_support.h"
using namespace Win2Air;
void TestLogger(wchar_t const* path) {
  auto root = std::filesystem::path(path);
  auto now = std::chrono::steady_clock::time_point{};
  Logger logger(root, [&] { return now; });
  Require(logger.Level() == DefaultLogLevel() &&
              ResolveLogLevel("") == DefaultLogLevel(),
          "A fresh logger uses the build-specific default");
#ifdef _DEBUG
  Require(logger.Level() == LogLevel::debug,
          "Debug builds default to Debug logging");
#else
  Require(logger.Level() == LogLevel::info,
          "Release builds default to Info logging");
#endif
  logger.SetLevel(LogLevel::trace);
  for (auto level : {LogLevel::trace, LogLevel::debug, LogLevel::info,
                     LogLevel::warning, LogLevel::error})
    logger.Write({level, LogModule::streaming, LogEvent::protocol_failed,
                  "AA:BB:CC:DD:EE:FF", 1, 4, 10054, 470});
  logger.Write(
      {LogLevel::debug, LogModule::audio, LogEvent::audio_timeline,
       "AA:BB:CC:DD:EE:FF", 3, 0, 0, 0, 1,
       "resume origin_100ns=10 now_100ns=20 rebase_100ns=10 age_ms=0"});
  logger.Write({LogLevel::info, LogModule::streaming,
                LogEvent::adaptive_latency, "AA:BB:CC:DD:EE:FF", 4, 0, 0, 0, 1,
                "reason=latency latency_ms=100 recovery_locked=0"});
  logger.Write({LogLevel::info, LogModule::streaming, LogEvent::media_control,
                "AA:BB:CC:DD:EE:FF", 4, 0, 0, 0, 1,
                "action=next outcome=queued"});
  Rejects(
      [&] {
        logger.Write({LogLevel::trace, LogModule::credentials,
                      LogEvent::callback_failed, "secret-token"});
      },
      "Arbitrary sensitive log fields rejected");
  logger.Flush();
  std::ifstream input(root / "app.log");
  std::string text((std::istreambuf_iterator<char>(input)), {});
  input.close();
  for (auto field : {"timestamp", "thread", "module", "receiver", "state",
                     "event", "code", "message"})
    Require(text.find(field) != std::string::npos,
            "Structured log field absent");
  Require(text.find("audio-timeline") != std::string::npos &&
              text.find("resume origin_100ns=10 now_100ns=20 rebase_100ns=10 "
                        "age_ms=0") != std::string::npos,
          "Audio timeline detail is retained in the structured log");
  Require(text.find("secret-token") == std::string::npos,
          "Rejected secret not persisted");
  Require(text.find("adaptive-latency") != std::string::npos &&
              text.find("reason=latency latency_ms=100 recovery_locked=0") !=
                  std::string::npos,
          "Latency changes retain their reason and recovery state");
  Require(text.find("media-control") != std::string::npos &&
              text.find("action=next outcome=queued") != std::string::npos,
          "Receiver media controls retain their action and outcome");
  {
    auto filtered_root =
        root.parent_path() / (root.filename().wstring() + L"-levels");
    Logger filtered(filtered_root);
    for (auto minimum : {LogLevel::trace, LogLevel::debug, LogLevel::info,
                         LogLevel::warning, LogLevel::error}) {
      filtered.SetLevel(minimum);
      for (auto severity : {LogLevel::trace, LogLevel::debug, LogLevel::info,
                            LogLevel::warning, LogLevel::error}) {
        LogRecord record;
        record.level = severity;
        record.detail = "minimum=" + std::string(LogLevelName(minimum)) +
                        " severity=" + std::string(LogLevelName(severity));
        filtered.Write(record);
      }
    }
    filtered.Flush();
    std::ifstream stream(filtered_root / L"app.log");
    std::string contents((std::istreambuf_iterator<char>(stream)), {});
    for (auto minimum : {LogLevel::trace, LogLevel::debug, LogLevel::info,
                         LogLevel::warning, LogLevel::error})
      for (auto severity : {LogLevel::trace, LogLevel::debug, LogLevel::info,
                            LogLevel::warning, LogLevel::error}) {
        auto marker = "minimum=" + std::string(LogLevelName(minimum)) +
                      " severity=" + std::string(LogLevelName(severity));
        Require((contents.find(marker) != std::string::npos) ==
                    (severity >= minimum),
                "Changing the threshold filters the next record immediately");
      }
    Rejects([&] { filtered.SetLevel(static_cast<LogLevel>(99)); },
            "Invalid thresholds are rejected");
    Require(filtered.Level() == LogLevel::error,
            "Invalid thresholds preserve the active filter");
    Rejects([&] { ResolveLogLevel("verbose"); },
            "Unknown persisted log levels are rejected");
  }
  {
    std::vector<LogRecord> records;
    LogDiagnostic([&](LogRecord const& record) { records.push_back(record); },
                  {}, std::string(5000, 'x') + "ignored-tail");
    Require(
        records.back().detail == "diagnostic text truncated after 4096 bytes" &&
            records.size() < 20,
        "Unexpectedly large failures cannot produce unbounded log output");
    records.clear();
    LogDiagnostic([&](LogRecord const& record) { records.push_back(record); },
                  {}, "quoted \"value\"\\path\n中文");
    for (auto const& record : records) {
      Require(record.detail.size() <= 240 &&
                  record.detail.find('\n') == std::string::npos,
              "Localized failures remain bounded single-line log records");
      logger.Write(record);
    }
    logger.Flush();
    std::ifstream stream(root / L"app.log");
    std::string line;
    while (std::getline(stream, line))
      (void)winrt::Windows::Data::Json::JsonObject::Parse(
          winrt::to_hstring(line));
  }
  std::filesystem::resize_file(root / "app.log", 10 * 1024 * 1024);
  logger.Write({});
  logger.Flush();
  Require(std::filesystem::exists(root / "app.1.log") &&
              std::filesystem::file_size(root / "app.log") < 1024,
          "Size rotation");
  std::ofstream(root / "app.2.log")
      << "{\"timestamp\":1,\"event\":\"expired\"}\n";
  logger.Write({});
  logger.Flush();
  Require(std::filesystem::exists(root / "app.2.log"),
          "A burst of control logs does not rescan retention on every write");
  now += std::chrono::minutes(1);
  logger.Write({});
  logger.Flush();
  Require(!std::filesystem::exists(root / "app.2.log"),
          "Expired logs pruned using record age");
  for (unsigned i = 0; i < 7; ++i) {
    std::filesystem::resize_file(root / "app.log", 10 * 1024 * 1024);
    logger.Write({});
    logger.Flush();
  }
  unsigned files{};
  for (auto const& item : std::filesystem::directory_iterator(root)) {
    (void)item;
    ++files;
  }
  Require(files <= 5, "Rotated log count bound");
  for (auto const& item : std::filesystem::directory_iterator(root))
    Require(item.file_size() <= 10 * 1024 * 1024,
            "Log file exceeds retention bound");
  {
    // Block the writer inside its storage cycle, then prove producers keep
    // running and overflow remains bounded and observable.
    std::promise<void> blocked, release;
    auto released = release.get_future().share();
    std::atomic<bool> hold{};
    auto async_root =
        root.parent_path() / (root.filename().wstring() + L"-async");
    std::thread::id storage_thread;
    Logger asynchronous(async_root, [&] {
      if (hold.exchange(false)) {
        storage_thread = std::this_thread::get_id();
        blocked.set_value();
        released.wait();
      }
      return std::chrono::steady_clock::now();
    });
    hold = true;
    asynchronous.Write({});
    auto started = blocked.get_future().wait_for(std::chrono::seconds(5));
    if (started != std::future_status::ready) {
      release.set_value();
      Require(false, "Storage worker reached the controlled write barrier");
    }
    auto produced = std::async(std::launch::async, [&] {
      for (unsigned i = 0; i < 5000; ++i) {
        LogRecord record;
        record.detail = "queued=" + std::to_string(i);
        asynchronous.Write(record);
      }
    });
    bool independent =
        produced.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    release.set_value();
    produced.get();
    asynchronous.Flush();
    Require(independent && storage_thread != std::this_thread::get_id(),
            "Log producers complete while the independent storage worker is "
            "blocked");
    std::ifstream stream(async_root / L"app.log");
    std::string contents((std::istreambuf_iterator<char>(stream)), {});
    Require(contents.find("queued=4095") != std::string::npos &&
                contents.find("queue_overflow dropped_records=904") !=
                    std::string::npos,
            "The bounded queue retains ordered records and reports overflow "
            "after storage resumes");
  }
  {
    auto exit_root =
        root.parent_path() / (root.filename().wstring() + L"-exit");
    {
      Logger exiting(exit_root);
      for (unsigned i = 0; i < 100; ++i) {
        LogRecord record;
        record.detail = "exit=" + std::to_string(i);
        exiting.Write(record);
      }
    }
    std::ifstream stream(exit_root / L"app.log");
    std::string line;
    unsigned count{};
    while (std::getline(stream, line)) ++count;
    Require(count == 100, "Shutdown drains every accepted log record");
  }
  {
    auto failed_root =
        root.parent_path() / (root.filename().wstring() + L"-failure");
    Logger failing(failed_root);
    std::filesystem::create_directory(failed_root / L"app.log");
    failing.Write({});
    Rejects([&] { failing.Flush(); },
            "Storage failure is propagated through the flush barrier");
    Rejects(
        [&] { failing.Write({}); },
        "A failed worker reports its original storage failure to producers");
  }
}
