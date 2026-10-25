#pragma once
#include <winrt/Windows.Data.Json.h>

#include "../Streaming/StreamCoordinator.h"
namespace Win2Air {
class SystemAudioValidation {
  StreamCoordinator& service_;
  std::filesystem::path output_, wave_;
  winrt::Windows::Data::Json::JsonObject report_;
  std::future<CommandResult> pending_;
  std::chrono::steady_clock::time_point deadline_, next_;
  unsigned step_{};
  uint64_t muted_nonzero_{};
  EndpointLevel original_;
  std::string endpoint_, identity_;
  int expected_base_{}, expected_offset_{};
  unsigned continuous_seconds_{};
  uint64_t continuous_generation_{}, previous_submitted_{}, previous_nonzero_{};
  winrt::Windows::Data::Json::JsonArray continuous_samples_;
  void Write(std::string const& status, std::string const& error = "");

 public:
  SystemAudioValidation(StreamCoordinator&,
                        std::filesystem::path const& config);
  bool Tick();
  ~SystemAudioValidation();
};
}  // namespace Win2Air
