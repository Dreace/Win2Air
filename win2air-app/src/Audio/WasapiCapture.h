#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <thread>

#include "DeviceManager.h"
#include "SpscRing.h"
namespace Win2Air {
struct CapturePacket {
  BYTE const* bytes{};
  UINT32 frames{};
  DWORD flags{};
  UINT64 position{}, timestamp_100ns{};
};
struct CapturePeriod {
  unsigned default_frames{}, minimum_frames{}, buffer_frames{};
  std::string reason;
};
class CaptureBackend {
 public:
  virtual ~CaptureBackend() = default;
  virtual std::vector<uint8_t> Open(std::string const& endpoint) = 0;
  virtual bool Next(CapturePacket& packet) = 0;
  virtual void Release(UINT32 frames) = 0;
  virtual void Stop() = 0;
  virtual CapturePeriod PeriodInfo() const {
    return {0, 0, 0, "Capture period was not observed"};
  }
};
CapturedBlock CopyCapturedPacket(BYTE const* bytes, UINT32 frames, DWORD flags,
                                 UINT64 position, UINT64 capture_100ns,
                                 WAVEFORMATEX const& format, uint64_t epoch);
class WasapiCapture {
  std::function<std::unique_ptr<CaptureBackend>()> backend_factory_;
  std::function<void()> on_ready_;
  std::thread worker_;
  std::atomic<bool> stopping_{};
  std::mutex mutex_;
  SpscRing<CapturedBlock, 64> blocks_;
  std::vector<uint8_t> format_;
  std::atomic<size_t> queued_bytes_{}, capacity_bytes_{};
  std::atomic<unsigned> buffer_ms_{250};
  std::atomic<uint64_t> dropped_blocks_{}, latest_end_100ns_{};
  std::atomic<bool> gap_pending_{};
  bool delivery_gap_{};
  std::exception_ptr failure_;
  CapturePeriod period_;
  void Run(std::string endpoint, uint64_t epoch, std::promise<void> started);

 public:
  explicit WasapiCapture(
      std::function<std::unique_ptr<CaptureBackend>()> factory = {})
      : backend_factory_(std::move(factory)) {}
  ~WasapiCapture();
  void Start(std::string endpoint, uint64_t epoch, unsigned buffer_ms = 250);
  void OnReady(std::function<void()> callback);
  void SetBufferBudget(unsigned buffer_ms);
  void CheckBufferBudget(unsigned buffer_ms);
  bool HoldsQueuedAudio(unsigned buffer_ms);
  size_t BufferCapacityBytes();
  void Stop();
  void Clear();
  std::optional<CapturedBlock> Pop();
  std::vector<uint8_t> Format();
  uint64_t DroppedBlocks();
  CapturePeriod PeriodInfo();
};
}  // namespace Win2Air
