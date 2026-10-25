#include "WasapiCapture.h"

#include <avrt.h>

#include <cstring>
namespace Win2Air {
namespace {
class NativeCaptureBackend final : public CaptureBackend {
  winrt::com_ptr<IAudioClient> client_;
  winrt::com_ptr<IAudioCaptureClient> capture_;
  winrt::handle event_;
  HANDLE mmcss_{};
  bool started_{};
  CapturePeriod period_;

 public:
  ~NativeCaptureBackend() override {
    if (client_ && started_) client_->Stop();
    if (mmcss_) AvRevertMmThreadCharacteristics(mmcss_);
  }
  std::vector<uint8_t> Open(std::string const& endpoint) override {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::check_hresult(
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
    winrt::com_ptr<IMMDevice> device;
    winrt::check_hresult(enumerator->GetDevice(
        winrt::to_hstring(endpoint).c_str(), device.put()));
    winrt::check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                          nullptr, client_.put_void()));
    WAVEFORMATEX* mix{};
    winrt::check_hresult(client_->GetMixFormat(&mix));
    std::vector<uint8_t> format;
    try {
      ValidateAudioFormat(*mix);
      auto client3 = client_.try_as<IAudioClient3>();
      if (client3) {
        UINT32 fundamental{}, maximum{};
        auto code = client3->GetSharedModeEnginePeriod(
            mix, &period_.default_frames, &fundamental, &period_.minimum_frames,
            &maximum);
        if (FAILED(code)) {
          period_.default_frames = period_.minimum_frames = 0;
          period_.reason =
              "GetSharedModeEnginePeriod failed: " + std::to_string(code);
        }
      } else {
        period_.reason = "IAudioClient3 engine period query is unavailable";
      }
      format.resize(sizeof(WAVEFORMATEX) + mix->cbSize);
      std::memcpy(format.data(), mix, format.size());
      event_.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
      if (!event_) winrt::throw_last_error();
      winrt::check_hresult(client_->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0,
          0, mix, nullptr));
    } catch (...) {
      CoTaskMemFree(mix);
      throw;
    }
    CoTaskMemFree(mix);
    winrt::check_hresult(client_->GetBufferSize(&period_.buffer_frames));
    winrt::check_hresult(client_->SetEventHandle(event_.get()));
    winrt::check_hresult(client_->GetService(__uuidof(IAudioCaptureClient),
                                             capture_.put_void()));
    DWORD task{};
    mmcss_ = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    if (!mmcss_) winrt::throw_last_error();
    if (!AvSetMmThreadPriority(mmcss_, AVRT_PRIORITY_HIGH))
      winrt::throw_last_error();
    winrt::check_hresult(client_->Start());
    started_ = true;
    return format;
  }
  bool Next(CapturePacket& packet) override {
    UINT32 count{};
    winrt::check_hresult(capture_->GetNextPacketSize(&count));
    if (!count) {
      if (WaitForSingleObject(event_.get(), 25) == WAIT_FAILED)
        winrt::throw_last_error();
      winrt::check_hresult(capture_->GetNextPacketSize(&count));
      if (!count) return false;
    }
    BYTE* bytes{};
    winrt::check_hresult(capture_->GetBuffer(&bytes, &packet.frames,
                                             &packet.flags, &packet.position,
                                             &packet.timestamp_100ns));
    packet.bytes = bytes;
    return true;
  }
  void Release(UINT32 frames) override {
    winrt::check_hresult(capture_->ReleaseBuffer(frames));
  }
  void Stop() override {
    if (started_) {
      started_ = false;
      winrt::check_hresult(client_->Stop());
    }
  }
  CapturePeriod PeriodInfo() const override { return period_; }
};
}  // namespace

CapturedBlock CopyCapturedPacket(BYTE const* bytes, UINT32 frames, DWORD flags,
                                 UINT64 position, UINT64 capture_100ns,
                                 WAVEFORMATEX const& format, uint64_t epoch) {
  auto parsed = ValidateAudioFormat(format);
  if (!epoch || !frames || frames > format.nSamplesPerSec ||
      (!bytes && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)))
    throw std::runtime_error(
        "Invalid capture packet or absent nonsilent driver buffer");
  CapturedBlock copy;
  copy.source_epoch = epoch;
  copy.device_frame_position = position;
  copy.capture_100ns = capture_100ns;
  copy.frame_count = frames;
  copy.flags = flags;
  copy.format = parsed;
  copy.owned_bytes.resize(size_t(frames) * format.nBlockAlign);
  if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
    std::memcpy(copy.owned_bytes.data(), bytes, copy.owned_bytes.size());
  return copy;
}
WasapiCapture::~WasapiCapture() { Stop(); }
void WasapiCapture::OnReady(std::function<void()> callback) {
  if (worker_.joinable())
    throw std::runtime_error("Capture notification requires a stopped stream");
  on_ready_ = std::move(callback);
}
void WasapiCapture::CheckBufferBudget(unsigned buffer_ms) {
  if (buffer_ms < 5 || buffer_ms > 250)
    throw std::runtime_error("Capture buffer budget must be 5 through 250 ms");
  std::lock_guard lock(mutex_);
  if (format_.size() < sizeof(WAVEFORMATEX))
    throw std::runtime_error("Capture buffer budget requires an opened format");
  auto const& wave = *reinterpret_cast<WAVEFORMATEX const*>(format_.data());
  auto frames = (size_t(wave.nSamplesPerSec) * buffer_ms + 999) / 1000;
  if (period_.default_frames > frames)
    throw std::runtime_error(
        "Capture engine period " + std::to_string(period_.default_frames) +
        " frames exceeds the requested " + std::to_string(buffer_ms) +
        " ms budget at " + std::to_string(wave.nSamplesPerSec) + " Hz");
}
bool WasapiCapture::HoldsQueuedAudio(unsigned buffer_ms) {
  std::lock_guard lock(mutex_);
  if (format_.size() < sizeof(WAVEFORMATEX) || buffer_ms < 5 || buffer_ms > 250)
    return false;
  auto const& wave = *reinterpret_cast<WAVEFORMATEX const*>(format_.data());
  if (!wave.nSamplesPerSec || !wave.nBlockAlign) return false;
  auto frames = (size_t(wave.nSamplesPerSec) * buffer_ms + 999) / 1000;
  if (period_.default_frames > frames) return false;
  auto capacity = frames * wave.nBlockAlign;
  return queued_bytes_ <= capacity;
}
void WasapiCapture::SetBufferBudget(unsigned buffer_ms) {
  CheckBufferBudget(buffer_ms);
  std::lock_guard lock(mutex_);
  auto const& wave = *reinterpret_cast<WAVEFORMATEX const*>(format_.data());
  buffer_ms_ = buffer_ms;
  capacity_bytes_ =
      (size_t(wave.nSamplesPerSec) * buffer_ms + 999) / 1000 * wave.nBlockAlign;
  bool dropped = false;
  while (queued_bytes_ > capacity_bytes_) {
    auto block = blocks_.Pop();
    if (!block) break;
    queued_bytes_ -= block->owned_bytes.size();
    ++dropped_blocks_;
    dropped = true;
  }
  if (dropped) {
    delivery_gap_ = true;
  }
}
size_t WasapiCapture::BufferCapacityBytes() {
  std::lock_guard lock(mutex_);
  return capacity_bytes_;
}
void WasapiCapture::Start(std::string endpoint, uint64_t epoch,
                          unsigned buffer_ms) {
  if (worker_.joinable() || endpoint.empty() || !epoch || buffer_ms < 5 ||
      buffer_ms > 250)
    throw std::runtime_error(
        "Capture requires a stopped stream and explicit endpoint epoch");
  stopping_ = false;
  buffer_ms_ = buffer_ms;
  failure_ = nullptr;
  std::promise<void> started;
  auto future = started.get_future();
  worker_ = std::thread([this, endpoint = std::move(endpoint), epoch,
                         promise = std::move(started)]() mutable {
    Run(std::move(endpoint), epoch, std::move(promise));
  });
  try {
    future.get();
  } catch (...) {
    stopping_ = true;
    worker_.join();
    throw;
  }
}
void WasapiCapture::Stop() {
  stopping_ = true;
  if (worker_.joinable()) worker_.join();
  std::lock_guard lock(mutex_);
  while (blocks_.Pop()) {
  }
  queued_bytes_ = 0;
}
void WasapiCapture::Clear() {
  std::lock_guard lock(mutex_);
  if (failure_) std::rethrow_exception(failure_);
  for (unsigned index = 0; index < 64; ++index) {
    auto block = blocks_.Pop();
    if (!block) break;
    queued_bytes_ -= block->owned_bytes.size();
  }
  delivery_gap_ = false;
}
std::optional<CapturedBlock> WasapiCapture::Pop() {
  std::lock_guard lock(mutex_);
  if (failure_) std::rethrow_exception(failure_);
  while (auto block = blocks_.Pop()) {
    queued_bytes_ -= block->owned_bytes.size();
    auto const& wave = *reinterpret_cast<WAVEFORMATEX const*>(format_.data());
    auto latest = latest_end_100ns_.load();
    auto age_limit = uint64_t(capacity_bytes_.load() / wave.nBlockAlign) *
                         10000000 / wave.nSamplesPerSec +
                     1;
    if (!(block->flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) &&
        latest > block->capture_100ns &&
        latest - block->capture_100ns > age_limit) {
      ++dropped_blocks_;
      delivery_gap_ = true;
      continue;
    }
    if (delivery_gap_) block->flags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
    delivery_gap_ = false;
    return block;
  }
  return {};
}
std::vector<uint8_t> WasapiCapture::Format() {
  std::lock_guard lock(mutex_);
  return format_;
}
uint64_t WasapiCapture::DroppedBlocks() {
  std::lock_guard lock(mutex_);
  return dropped_blocks_;
}
CapturePeriod WasapiCapture::PeriodInfo() {
  std::lock_guard lock(mutex_);
  return period_;
}
void WasapiCapture::Run(std::string endpoint, uint64_t epoch,
                        std::promise<void> started) {
  bool opened = false, apartment = false;
  std::unique_ptr<CaptureBackend> backend;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    apartment = true;
    backend = backend_factory_ ? backend_factory_()
                               : std::make_unique<NativeCaptureBackend>();
    if (!backend)
      throw std::runtime_error("Capture backend factory returned no driver");
    auto format = backend->Open(endpoint);
    if (format.size() < sizeof(WAVEFORMATEX))
      throw std::runtime_error("Capture backend returned a truncated format");
    auto const& wave = *reinterpret_cast<WAVEFORMATEX const*>(format.data());
    if (format.size() < sizeof(WAVEFORMATEX) + wave.cbSize)
      throw std::runtime_error(
          "Capture backend returned truncated format extensions");
    ValidateAudioFormat(wave);
    {
      std::lock_guard lock(mutex_);
      format_ = std::move(format);
      period_ = backend->PeriodInfo();
      capacity_bytes_ = (size_t(wave.nSamplesPerSec) * buffer_ms_ + 999) /
                        1000 * wave.nBlockAlign;
      if (period_.default_frames > capacity_bytes_ / wave.nBlockAlign)
        throw std::runtime_error(
            "Capture engine period " + std::to_string(period_.default_frames) +
            " frames exceeds the requested " +
            std::to_string(buffer_ms_.load()) + " ms budget at " +
            std::to_string(wave.nSamplesPerSec) + " Hz");
      dropped_blocks_ = 0;
      latest_end_100ns_ = 0;
      delivery_gap_ = false;
      gap_pending_ = false;
    }
    opened = true;
    started.set_value();
    while (!stopping_) {
      CapturePacket packet;
      if (!backend->Next(packet)) continue;
      CapturedBlock copy;
      try {
        copy = CopyCapturedPacket(packet.bytes, packet.frames, packet.flags,
                                  packet.position, packet.timestamp_100ns, wave,
                                  epoch);
      } catch (...) {
        backend->Release(packet.frames);
        throw;
      }
      backend->Release(packet.frames);
      if (!(copy.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
        auto duration =
            uint64_t(copy.frame_count) * 10000000 / wave.nSamplesPerSec;
        if (copy.capture_100ns > UINT64_MAX - duration)
          throw std::runtime_error("Capture packet timestamp overflows");
        latest_end_100ns_ = copy.capture_100ns + duration;
      }
      auto capacity = capacity_bytes_.load();
      auto bytes = copy.owned_bytes.size();
      if (bytes > capacity)
        throw std::runtime_error(
            "Capture packet " + std::to_string(copy.frame_count) +
            " frames exceeds the requested " +
            std::to_string(buffer_ms_.load()) + " ms budget at " +
            std::to_string(wave.nSamplesPerSec) + " Hz");
      if (bytes <= capacity && queued_bytes_.load() <= capacity - bytes) {
        queued_bytes_ += bytes;
        if (gap_pending_.exchange(false))
          copy.flags |= AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
        if (!blocks_.Push(copy)) {
          queued_bytes_ -= bytes;
          ++dropped_blocks_;
          gap_pending_ = true;
        }
      } else {
        ++dropped_blocks_;
        gap_pending_ = true;
      }
      if (on_ready_) on_ready_();
    }
  } catch (...) {
    auto error = std::current_exception();
    if (!opened)
      started.set_exception(error);
    else {
      std::lock_guard lock(mutex_);
      failure_ = error;
    }
    if (on_ready_) on_ready_();
  }
  if (backend) {
    try {
      backend->Stop();
    } catch (...) {
      std::lock_guard lock(mutex_);
      if (!failure_) failure_ = std::current_exception();
    }
  }
  backend.reset();
  if (apartment) winrt::uninit_apartment();
}
}  // namespace Win2Air
