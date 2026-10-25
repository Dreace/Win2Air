#pragma once
#include "SessionModels.h"
namespace Win2Air {
class ReconnectController {
  ReconnectState state_;
  bool waiting_{};
  void Arm(uint64_t now, uint64_t epoch, std::string reason, unsigned attempt);

 public:
  void Schedule(uint64_t now, uint64_t epoch, std::string reason);
  void Resume(uint64_t now, uint64_t epoch, std::string reason);
  bool Due(uint64_t now, uint64_t epoch) const noexcept;
  void Cancel() noexcept;
  void Reset() noexcept;
  std::optional<ReconnectState> Snapshot() const;
};
}  // namespace Win2Air
