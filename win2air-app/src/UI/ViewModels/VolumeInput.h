#pragma once
#include <cmath>
#include <future>
#include <optional>
#include "../../Streaming/SessionModels.h"
#include "SettingDraft.h"
namespace Win2Air {
class VolumeInput {
  SettingDraft<double> draft_;
  std::optional<std::future<CommandResult>> pending_;
  uint64_t sent_version_{};
  double sent_value_{};

 public:
  void Update(double confirmed) { draft_.Update(confirmed); }
  double Value() const { return draft_.Value(); }
  void Edit(double value) {
    if (!std::isfinite(value) || value < 0 || value > 1)
      throw std::invalid_argument("Volume must be within zero to one");
    draft_.Edit(value);
  }
  std::optional<CommandResult> Complete() {
    if (!pending_ || pending_->wait_for(std::chrono::seconds(0)) !=
                         std::future_status::ready)
      return {};
    auto result = pending_->get();
    pending_.reset();
    if (result.status == CommandStatus::applied)
      draft_.Acknowledge(sent_version_, sent_value_);
    else if (draft_.Version() == sent_version_)
      draft_ = {};
    return result;
  }
  template <class Submit>
  void Send(Submit submit) {
    if (pending_ || !draft_.Dirty()) return;
    sent_version_ = draft_.Version();
    sent_value_ = draft_.Value();
    pending_ = submit(sent_value_);
  }
};
}  // namespace Win2Air
