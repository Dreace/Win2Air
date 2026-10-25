#pragma once
#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>

#include "../Streaming/SessionModels.h"
namespace Win2Air {
struct EndpointLevel {
  float volume{};
  bool muted{};
};
class EndpointVolume {
 public:
  virtual ~EndpointVolume() = default;
  virtual EndpointLevel Read() = 0;
  virtual void Mute(bool value, GUID const& context) = 0;
  virtual void Volume(float value, GUID const& context) = 0;
  virtual bool ExternalChange() { return false; }
};
using EndpointFactory =
    std::function<std::unique_ptr<EndpointVolume>(std::string const&)>;
std::unique_ptr<EndpointVolume> OpenEndpointVolume(std::string const& endpoint);
class LocalPlaybackController {
  std::filesystem::path file_;
  EndpointFactory factory_;
  std::optional<LocalPlaybackLease> lease_;
  std::unique_ptr<EndpointVolume> target_;
  void Persist();

 public:
  explicit LocalPlaybackController(
      std::filesystem::path root, EndpointFactory factory = OpenEndpointVolume);
  void Recover();
  void Disable(std::string const& endpoint, uint64_t epoch);
  void Restore();
  void ObserveExternalChange();
  void PollChanges();
  std::optional<LocalPlaybackLease> Lease() const { return lease_; }
};
}  // namespace Win2Air
