#include "AudioFixture.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "../../src/Streaming/StreamCoordinator.h"
namespace Win2Air::Tests {
std::vector<uint8_t> WaveFormat(unsigned rate) {
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nSamplesPerSec = rate;
  format.nChannels = 2;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 4;
  format.nAvgBytesPerSec = rate * 4;
  std::vector<uint8_t> bytes(sizeof(format));
  std::memcpy(bytes.data(), &format, sizeof(format));
  return bytes;
}
VirtualPacket NonzeroPacket(unsigned frames, uint64_t timestamp) {
  VirtualPacket result;
  result.frames = frames;
  result.timestamp = timestamp ? timestamp : MonotonicNs() / 100;
  result.bytes.resize(size_t(frames) * 4);
  for (size_t i = 0; i < result.bytes.size(); i += 2) {
    result.bytes[i] = 0;
    result.bytes[i + 1] = 16;
  }
  return result;
}
VirtualAudioState::VirtualAudioState()
    : devices{{"virtual-default", "Virtual default output", true, WaveFormat()},
              {"virtual-specified", "Virtual specified output", false,
               WaveFormat(48000)}},
      levels{{"virtual-default", {0.65f, false}},
             {"virtual-specified", {0.25f, true}}} {}
void VirtualAudioState::Push(VirtualPacket packet) {
  std::lock_guard lock(mutex);
  packets.push_back(std::move(packet));
  ready.notify_all();
}
void VirtualAudioState::Notify(DeviceEvent event) {
  std::lock_guard lock(mutex);
  if (notifications) notifications->Notify(std::move(event));
}
void VirtualAudioState::Remove(std::string const& id) {
  {
    std::lock_guard lock(mutex);
    std::erase_if(devices, [&](auto const& item) { return item.id == id; });
  }
  Notify({id, DEVICE_STATE_NOTPRESENT, false, false});
}
void VirtualAudioState::ChangeDefault(std::string const& id) {
  {
    std::lock_guard lock(mutex);
    for (auto& item : devices) item.is_default = item.id == id;
  }
  Notify({id, DEVICE_STATE_ACTIVE, true, false});
}
VirtualDevices::VirtualDevices(std::shared_ptr<VirtualAudioState> state)
    : DeviceManager(false), state_(std::move(state)) {
  std::lock_guard lock(state_->mutex);
  state_->notifications = this;
}
VirtualDevices::~VirtualDevices() {
  std::lock_guard lock(state_->mutex);
  state_->notifications = nullptr;
}
std::vector<AudioDevice> VirtualDevices::Enumerate() {
  std::lock_guard lock(state_->mutex);
  return state_->devices;
}
void VirtualCaptureBackend::CheckOwner() {
  if (owner_ != std::this_thread::get_id()) {
    state_->wrong_thread = true;
    throw std::runtime_error(
        "Virtual audio driver operation used a different thread");
  }
}
std::vector<uint8_t> VirtualCaptureBackend::Open(std::string const& endpoint) {
  std::lock_guard lock(state_->mutex);
  owner_ = std::this_thread::get_id();
  if (state_->fail_open)
    throw std::runtime_error("Virtual audio driver failed to open");
  auto device =
      std::find_if(state_->devices.begin(), state_->devices.end(),
                   [&](auto const& value) { return value.id == endpoint; });
  if (device == state_->devices.end())
    throw std::runtime_error("Virtual audio endpoint is absent");
  opened_ = true;
  state_->active_endpoint = endpoint;
  ++state_->opened;
  return device->mix_format;
}
bool VirtualCaptureBackend::Next(CapturePacket& packet) {
  std::unique_lock lock(state_->mutex);
  CheckOwner();
  if (state_->fail_next)
    throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED));
  if (borrowed_)
    throw std::runtime_error(
        "Driver buffer was not released before acquiring the next packet");
  state_->ready.wait_for(lock, std::chrono::milliseconds(10),
                         [&] { return !state_->packets.empty(); });
  if (state_->packets.empty()) return false;
  borrowed_ = std::move(state_->packets.front());
  state_->packets.pop_front();
  packet = {borrowed_->bytes.empty() ? nullptr : borrowed_->bytes.data(),
            borrowed_->frames, borrowed_->flags, borrowed_->position,
            borrowed_->timestamp};
  return true;
}
void VirtualCaptureBackend::Release(UINT32 frames) {
  std::lock_guard lock(state_->mutex);
  CheckOwner();
  if (!borrowed_ || frames != borrowed_->frames)
    throw std::runtime_error(
        "Virtual driver buffer release did not match acquisition");
  std::fill(borrowed_->bytes.begin(), borrowed_->bytes.end(), uint8_t{0xCC});
  borrowed_.reset();
  ++state_->released;
  if (state_->fail_release)
    throw std::runtime_error("Virtual audio driver release failed");
}
void VirtualCaptureBackend::Stop() {
  std::lock_guard lock(state_->mutex);
  CheckOwner();
  if (opened_) {
    opened_ = false;
    ++state_->stopped;
    state_->active_endpoint.clear();
  }
}
namespace {
class VirtualEndpoint final : public EndpointVolume {
  std::shared_ptr<VirtualAudioState> state_;
  std::string id_;

 public:
  VirtualEndpoint(std::shared_ptr<VirtualAudioState> state, std::string id)
      : state_(std::move(state)), id_(std::move(id)) {}
  EndpointLevel Read() override {
    std::lock_guard lock(state_->mutex);
    return state_->levels.at(id_);
  }
  void Mute(bool value, GUID const&) override {
    std::lock_guard lock(state_->mutex);
    if (!value && state_->fail_unmute)
      throw std::runtime_error("Virtual endpoint restore failed");
    state_->levels.at(id_).muted = value;
    ++state_->mute_writes;
  }
  void Volume(float value, GUID const&) override {
    std::lock_guard lock(state_->mutex);
    state_->levels.at(id_).volume = value;
    ++state_->volume_writes;
  }
  bool ExternalChange() override {
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->external_change, false);
  }
};
}  // namespace
AudioServices VirtualAudioServices(std::shared_ptr<VirtualAudioState> state) {
  return {{},
          {},
          [state] { return std::make_unique<VirtualDevices>(state); },
          [state] {
            return std::make_unique<WasapiCapture>([state] {
              return std::make_unique<VirtualCaptureBackend>(state);
            });
          },
          [state](auto const& path) {
            return std::make_unique<LocalPlaybackController>(
                path, [state](auto const& id) {
                  return std::make_unique<VirtualEndpoint>(state, id);
                });
          }};
}
std::vector<int16_t> AudioFixture::Next(unsigned frames) {
  if (frames > 44100)
    throw std::runtime_error("Audio fixture request exceeds fixed bound");
  std::vector<int16_t> samples(frames * 2);
  for (unsigned i = 0; i < frames; ++i)
    samples[i * 2] = samples[i * 2 + 1] =
        ((sample_index + i) % 100 < 50 ? int16_t(4096) : int16_t(-4096));
  sample_index += frames;
  monotonic_ns += uint64_t(frames) * 1000000000 / 44100;
  return samples;
}
void EndpointFixture::SetMute(bool value) {
  if (!available) throw std::runtime_error("Fixture endpoint unavailable");
  muted = value;
  ++writes;
}
}  // namespace Win2Air::Tests
