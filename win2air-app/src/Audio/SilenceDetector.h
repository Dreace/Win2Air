#pragma once
#include "../Streaming/SessionModels.h"
namespace Win2Air {
class SilenceDetector {
  int timeout_{30};
  uint64_t since_{}, observed_{}, source_{}, next_{};
  unsigned frames_{}, voiced_{};
  double energy_[2]{};
  bool continuous_{}, silent_{true};

 public:
  void Configure(int seconds, uint64_t now);
  void Discontinuity(uint64_t now);
  bool Consume(ConvertedBlock const& block, uint64_t now);
  bool Due(uint64_t now) const;
  bool Pending(uint64_t now) const;
};
}  // namespace Win2Air
