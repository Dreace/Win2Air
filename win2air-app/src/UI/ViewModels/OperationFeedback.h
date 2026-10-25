#pragma once
#include <chrono>
#include <future>
#include <map>
#include <string>
#include <vector>
#include "../../Streaming/SessionModels.h"

namespace Win2Air {
// An empty target denotes an application-wide operation. Device feedback keeps
// its original target even when selection or connection intent changes.
class OperationFeedback {
  struct Feedback {
    uint64_t revision{};
    std::string error;
  };
  struct Pending {
    std::string target;
    uint64_t revision;
    std::future<CommandResult> result;
  };
  std::map<std::string, Feedback> feedback_;
  std::vector<Pending> pending_;

 public:
  uint64_t Begin(std::string const& target) {
    auto& value = feedback_[target];
    value.error.clear();
    return ++value.revision;
  }
  void Complete(std::string const& target, uint64_t revision,
                CommandResult const& result) {
    auto found = feedback_.find(target);
    if (found == feedback_.end() || found->second.revision != revision) return;
    if (result.status == CommandStatus::failed ||
        result.status == CommandStatus::rejected)
      found->second.error = result.message;
    else if (result.status == CommandStatus::applied)
      found->second.error.clear();
  }
  void Submit(std::future<CommandResult> result, std::string target = {}) {
    auto revision = Begin(target);
    pending_.push_back({std::move(target), revision, std::move(result)});
  }
  void Refresh() {
    for (auto item = pending_.begin(); item != pending_.end();) {
      if (item->result.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        ++item;
        continue;
      }
      Complete(item->target, item->revision, item->result.get());
      item = pending_.erase(item);
    }
  }
  void Report(std::string message, std::string const& target = {}) {
    Begin(target);
    feedback_.at(target).error = std::move(message);
  }
  std::string const& Error(std::string const& target = {}) const {
    static std::string const empty;
    auto found = feedback_.find(target);
    return found == feedback_.end() ? empty : found->second.error;
  }
  bool Pending() const noexcept { return !pending_.empty(); }
};
}  // namespace Win2Air
