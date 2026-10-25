#pragma once
#include <mftransform.h>

#include <functional>

#include "AudioClock.h"
#include "DeviceManager.h"
namespace Win2Air {
class SourceDiscontinuity : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};
class FormatConverter {
 public:
  using TransformFactory = std::function<winrt::com_ptr<IMFTransform>()>;

 private:
  TransformFactory factory_;
  std::function<uint64_t()> now_;
  winrt::com_ptr<IMFTransform> transform_;
  winrt::com_ptr<IMFMediaBuffer> output_buffer_;
  MFT_OUTPUT_STREAM_INFO output_info_{};
  AudioClock clock_;
  AudioFormat input_;
  uint64_t source_epoch_{}, timeline_epoch_{}, next_sample_{}, input_frames_{};
  uint64_t last_device_position_{};
  uint32_t last_device_frames_{};
  bool started_{}, anchored_{}, direct_pcm_{};
  std::vector<ConvertedBlock> Pull();
  void RememberDevicePosition(CapturedBlock const& input);

 public:
  bool DevicePositionContinues(CapturedBlock const& input) const;
  explicit FormatConverter(TransformFactory factory = {},
                           std::function<uint64_t()> now = MonotonicNs);
  ~FormatConverter();
  void Open(WAVEFORMATEX const& format, uint64_t source_epoch);
  std::vector<ConvertedBlock> Convert(CapturedBlock const& input);
  std::vector<ConvertedBlock> Drain();
  void Flush(uint64_t source_epoch);
  uint64_t TimelineEpoch() const noexcept { return timeline_epoch_; }
};
}  // namespace Win2Air
