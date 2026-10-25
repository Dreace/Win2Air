#pragma once
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
namespace Win2Air {
enum class AppPage { playback, devices, statistics, settings };
struct ActivationRequest {
  AppPage page{AppPage::playback};
  std::optional<std::string> receiver_id;
  bool disconnect{}, minimized{}, startup{}, explicit_page{};
};
ActivationRequest ParseActivation(std::wstring const& arguments);
ActivationRequest ParseProtocol(std::wstring const& uri);
std::wstring LaunchArguments(std::wstring const& arguments,
                             std::wstring const& executable);
class ActivationQueue {
  std::mutex mutex_;
  std::deque<ActivationRequest> pending_;
  bool closed_{};

 public:
  void Submit(ActivationRequest request);
  std::vector<ActivationRequest> Take();
  void Close();
};
}  // namespace Win2Air
