#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "../../src/Audio/LocalPlaybackController.h"
#include "../../src/Audio/WasapiCapture.h"
namespace Win2Air {
struct AudioServices;
}
namespace Win2Air::Tests {
struct VirtualPacket {
  std::vector<uint8_t> bytes;
  UINT32 frames{};
  DWORD flags{};
  uint64_t position{}, timestamp{};
};
struct VirtualAudioState {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<AudioDevice> devices;
  std::map<std::string, EndpointLevel> levels;
  std::deque<VirtualPacket> packets;
  DeviceManager* notifications{};
  std::string active_endpoint;
  CapturePeriod period;
  unsigned opened{}, released{}, stopped{}, mute_writes{}, volume_writes{};
  bool fail_open{}, fail_next{}, fail_release{}, wrong_thread{},
      external_change{}, fail_unmute{};
  VirtualAudioState();
  void Push(VirtualPacket packet);
  void Notify(DeviceEvent event);
  void Remove(std::string const& id);
  void ChangeDefault(std::string const& id);
};
class VirtualDevices final : public DeviceManager {
  std::shared_ptr<VirtualAudioState> state_;

 public:
  explicit VirtualDevices(std::shared_ptr<VirtualAudioState> state);
  ~VirtualDevices() override;
  std::vector<AudioDevice> Enumerate() override;
};
class VirtualCaptureBackend final : public CaptureBackend {
  std::shared_ptr<VirtualAudioState> state_;
  std::thread::id owner_;
  std::optional<VirtualPacket> borrowed_;
  bool opened_{};
  void CheckOwner();

 public:
  explicit VirtualCaptureBackend(std::shared_ptr<VirtualAudioState> state)
      : state_(std::move(state)) {}
  std::vector<uint8_t> Open(std::string const& endpoint) override;
  bool Next(CapturePacket& packet) override;
  void Release(UINT32 frames) override;
  void Stop() override;
  CapturePeriod PeriodInfo() const override { return state_->period; }
};
AudioServices VirtualAudioServices(std::shared_ptr<VirtualAudioState> state);
std::vector<uint8_t> WaveFormat(unsigned rate = 44100);
VirtualPacket NonzeroPacket(unsigned frames, uint64_t timestamp_100ns = 0);
struct AudioFixture {
  uint64_t monotonic_ns{1000000000}, sample_index{};
  std::vector<int16_t> Next(unsigned frames);
};
struct EndpointFixture {
  bool muted{}, available{true};
  double volume{0.35};
  unsigned writes{};
  void SetMute(bool value);
};
}  // namespace Win2Air::Tests
