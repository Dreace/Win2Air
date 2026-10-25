#include "../../src/Diagnostics/DiagnosticsExporter.h"
#include "../../src/Diagnostics/Logger.h"
#include "test_support.h"
using namespace Win2Air;
void TestDiagnosticsSecrets(wchar_t const* directory) {
  auto root = std::filesystem::path(directory);
  std::filesystem::create_directories(root);
  for (auto text :
       {R"({"password":"hidden"})", R"({"error":{"AuTh_Key":"hidden"}})",
        R"({"trace":[{"pair-ing-code":"1234"}]})",
        R"({"message":"Bearer hidden"})", R"({"message":"PIN=1234"})",
        R"({"message":"-----BEGIN PRIVATE KEY-----"})"})
    Rejects([&] { CheckDiagnosticText(text); },
            "Sensitive keys and nested secret values must be rejected");
  auto secret = std::string(192, 'a');
  Rejects([&] { CheckDiagnosticText("{\"error\":\"" + secret + "\"}"); },
          "Opaque pairing material cannot pass as an error message");
  Logger logger(root / L"logs");
  for (auto level : {LogLevel::trace, LogLevel::debug, LogLevel::info,
                     LogLevel::warning, LogLevel::error})
    logger.Write({level, LogModule::streaming,
                  LogEvent::protocol_state_observed, "AA:BB:CC:DD:EE:FF", 3,
                  0});
  logger.Flush();
  auto logs = DiagnosticsExporter::ReadLogs(root / L"logs");
  CheckDiagnosticText(logs, true);
  PlaybackSnapshot state;
  auto files = BuildDiagnostics(state, logs);
  DiagnosticsExporter::Export(root / L"valid.zip", files);
  Require(std::filesystem::exists(root / L"valid.zip"),
          "Complete safe diagnostics produce a committed standard ZIP");
  for (auto const& [name, unused] : files) {
    (void)unused;
    auto tainted = files;
    tainted[name] = name == "app.log"
                        ? "{\"level\":\"Trace\",\"message\":\"token=hidden\"}\n"
                        : "{\"error\":{\"private_key\":\"hidden\"}}";
    auto destination = root / (name + ".zip");
    Rejects([&] { DiagnosticsExporter::Export(destination, tainted); },
            "Every export file is checked instead of trusting the manifest");
    Require(!std::filesystem::exists(destination),
            "Rejected diagnostics never create a successful archive");
  }
  auto over_limit = files;
  over_limit["app.log"].assign(DiagnosticsExporter::kLimit + 1, ' ');
  Rejects([&] { DiagnosticsExporter::Export(root / L"large.zip", over_limit); },
          "Archives over the byte limit are rejected before text parsing");
  auto wrong_file = files;
  wrong_file["credentials.bin"] = "hidden";
  Rejects(
      [&] { DiagnosticsExporter::Export(root / L"private.zip", wrong_file); },
      "Files outside the public diagnostic whitelist are rejected");
  Rejects(
      [&] {
        DiagnosticsExporter::Export(root / L"missing" / L"failed.zip", files);
      },
      "Destination write failure is not reported as export success");
  unsigned calls{};
  Rejects(
      [&] {
        DiagnosticsExporter::Export(root / L"canceled.zip", files,
                                    [&] { return ++calls > 15; });
      },
      "Export cancellation rejects the transaction");
  Require(!std::filesystem::exists(root / L"canceled.zip"),
          "Canceled export cannot leave a committed archive");
  bool writing{};
  Rejects(
      [&] {
        DiagnosticsExporter::Export(root / L"write-canceled.zip", files, [&] {
          for (auto const& item : std::filesystem::directory_iterator(root))
            if (item.path().extension() == L".partial") writing = true;
          return writing;
        });
      },
      "Cancellation after opening the archive rejects the write transaction");
  Require(writing && !std::filesystem::exists(root / L"write-canceled.zip"),
          "Cancellation is exercised during an actual temporary archive write");
  for (auto const& item : std::filesystem::directory_iterator(root))
    Require(item.path().extension() != L".partial",
            "Write failure and cancellation clean their temporary archive");
  auto invalid = state;
  auto history = std::make_shared<MetricsHistory>();
  auto sample = std::make_shared<MetricsSnapshot>();
  sample->clock_lock.value = 0;
  history->snapshots.push_back(sample);
  invalid.metrics_history = history;
  Rejects(
      [&] { BuildDiagnostics(invalid, ""); },
      "Unavailable metrics reject a fabricated zero instead of serializing it");
}
