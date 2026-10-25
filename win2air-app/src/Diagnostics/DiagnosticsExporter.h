#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include "../Streaming/StreamCoordinator.h"
namespace Win2Air {
using DiagnosticFiles = std::map<std::string, std::string>;
void CheckDiagnosticText(
    std::string const& text, bool lines = false,
    std::function<bool()> canceled = [] { return false; });
DiagnosticFiles BuildDiagnostics(
    PlaybackSnapshot const&, std::string logs,
    std::function<bool()> canceled = [] { return false; });
class DiagnosticsExporter {
 public:
  static constexpr uint64_t kLimit = 100 * 1024 * 1024;
  static void Export(
      std::filesystem::path const&, DiagnosticFiles const&,
      std::function<bool()> canceled = [] { return false; });
  static std::string ReadLogs(
      std::filesystem::path const&,
      std::function<bool()> canceled = [] { return false; });
};
}  // namespace Win2Air
