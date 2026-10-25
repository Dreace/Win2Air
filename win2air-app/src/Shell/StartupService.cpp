#include "StartupService.h"
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <stdexcept>
#include "../Streaming/FailureDetail.h"
namespace Win2Air {
namespace {
template <class Operation>
auto Completed(Operation const& operation,
               std::chrono::steady_clock::time_point deadline) {
  auto remaining = deadline - std::chrono::steady_clock::now();
  if (remaining < decltype(remaining)::zero())
    remaining = decltype(remaining)::zero();
  if (operation.wait_for(
          std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(
              remaining)) == winrt::Windows::Foundation::AsyncStatus::Started) {
    operation.Cancel();
    throw std::runtime_error("Startup system operation exceeded its deadline");
  }
  return operation.GetResults();
}
StartupState State(winrt::Windows::ApplicationModel::StartupTaskState value) {
  using System = winrt::Windows::ApplicationModel::StartupTaskState;
  switch (value) {
    case System::Disabled:
      return StartupState::disabled;
    case System::Enabled:
      return StartupState::enabled;
    case System::DisabledByUser:
      return StartupState::disabled_by_user;
    case System::DisabledByPolicy:
      return StartupState::disabled_by_policy;
    case System::EnabledByPolicy:
      return StartupState::enabled_by_policy;
    default:
      throw std::runtime_error("Unsupported system startup state");
  }
}
class SystemStartup final : public StartupBackend {
  bool packaged_;
  std::chrono::steady_clock::time_point deadline_;

 public:
  explicit SystemStartup(bool packaged) : packaged_(packaged) {}
  StartupState Read() override {
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    if (!packaged_) return StartupState::unavailable;
    return State(
        Completed(winrt::Windows::ApplicationModel::StartupTask::GetAsync(
                      L"Win2AirStartup"),
                  deadline_)
            .State());
  }
  StartupState Enable() override {
    auto task =
        Completed(winrt::Windows::ApplicationModel::StartupTask::GetAsync(
                      L"Win2AirStartup"),
                  deadline_);
    return State(Completed(task.RequestEnableAsync(), deadline_));
  }
  StartupState Disable() override {
    auto task =
        Completed(winrt::Windows::ApplicationModel::StartupTask::GetAsync(
                      L"Win2AirStartup"),
                  deadline_);
    task.Disable();
    return State(task.State());
  }
};
template <class Action>
std::future<StartupResult> Run(std::shared_ptr<StartupBackend> backend,
                               Action action) {
  return std::async(std::launch::async, [backend = std::move(backend), action] {
    bool apartment{};
    StartupResult result;
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      apartment = true;
      result = action(*backend);
    } catch (...) {
      result = {StartupState::error, false, DescribeFailure().message};
    }
    if (apartment) winrt::uninit_apartment();
    return result;
  });
}
}  // namespace
StartupService::StartupService(std::shared_ptr<StartupBackend> backend)
    : backend_(std::move(backend)) {
  if (!backend_)
    throw std::runtime_error("Startup service requires a system backend");
}
std::future<StartupResult> StartupService::Query() {
  return Run(backend_, [](auto& backend) {
    return StartupResult{backend.Read(), false, ""};
  });
}
std::future<StartupResult> StartupService::SetEnabled(
    bool enabled, std::function<void(bool)> persist) {
  return Run(backend_, [enabled, persist = std::move(persist)](auto& backend) {
    auto current = backend.Read();
    if (current == StartupState::unavailable ||
        current == StartupState::error ||
        current == StartupState::disabled_by_policy ||
        (enabled && current == StartupState::disabled_by_user) ||
        (!enabled && current == StartupState::enabled_by_policy))
      return StartupResult{current, false,
                           "System startup policy or application identity "
                           "prevents the requested change"};
    auto state = enabled ? backend.Enable() : backend.Disable();
    bool actual = state == StartupState::enabled ||
                  state == StartupState::enabled_by_policy;
    if (actual == enabled && persist) {
      try {
        persist(enabled);
      } catch (...) {
        std::string error = DescribeFailure().message;
        try {
          state = current == StartupState::enabled ? backend.Enable()
                                                   : backend.Disable();
          if (state != current)
            error +=
                "; system startup rollback did not restore its prior state";
        } catch (...) {
          error += "; system startup rollback: " + DescribeFailure().message;
          state = StartupState::error;
        }
        return StartupResult{state, false, error};
      }
    }
    return StartupResult{
        state, actual == enabled,
        actual == enabled ? ""
                          : "System did not apply the requested startup state"};
  });
}
std::shared_ptr<StartupBackend> NativeStartupBackend(bool packaged) {
  return std::make_shared<SystemStartup>(packaged);
}
}  // namespace Win2Air
