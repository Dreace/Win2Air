#pragma once
#include <functional>
#include <future>
#include <memory>
#include <string>
namespace Win2Air {
enum class StartupState {
  disabled,
  enabled,
  disabled_by_user,
  disabled_by_policy,
  enabled_by_policy,
  unavailable,
  error
};
struct StartupResult {
  StartupState state{StartupState::disabled};
  bool applied{};
  std::string error;
};
class StartupBackend {
 public:
  virtual ~StartupBackend() = default;
  virtual StartupState Read() = 0;
  virtual StartupState Enable() = 0;
  virtual StartupState Disable() = 0;
};
class StartupService {
  std::shared_ptr<StartupBackend> backend_;

 public:
  explicit StartupService(std::shared_ptr<StartupBackend> backend);
  std::future<StartupResult> Query();
  std::future<StartupResult> SetEnabled(bool enabled,
                                        std::function<void(bool)> persist = {});
};
std::shared_ptr<StartupBackend> NativeStartupBackend(bool packaged);
}  // namespace Win2Air
