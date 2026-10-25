#include "DeviceManager.h"

#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>

#include <bit>
#include <cstring>
namespace Win2Air {
struct DeviceEventsState {
  std::mutex mutex;
  std::vector<DeviceEvent> events;
  std::atomic<bool> failed{}, closed{};
};
namespace {
void EnqueueDeviceEvent(std::shared_ptr<DeviceEventsState> const &state,
                        DeviceEvent event) noexcept {
  try {
    std::lock_guard lock(state->mutex);
    if (state->closed) return;
    if (state->events.size() >= 256 ||
        (event.endpoint_id.empty() && !event.default_changed)) {
      state->failed = true;
      return;
    }
    state->events.push_back(std::move(event));
  } catch (...) {
    state->failed = true;
  }
}
struct Notifications : winrt::implements<Notifications, IMMNotificationClient> {
  std::shared_ptr<DeviceEventsState> events;
  explicit Notifications(std::shared_ptr<DeviceEventsState> state)
      : events(std::move(state)) {}
  HRESULT __stdcall OnDeviceStateChanged(LPCWSTR id,
                                         DWORD state) noexcept override {
    try {
      EnqueueDeviceEvent(events, {winrt::to_string(id), state, false, false});
    } catch (...) {
      EnqueueDeviceEvent(events, {});
    }
    return S_OK;
  }
  HRESULT __stdcall OnDeviceAdded(LPCWSTR id) noexcept override {
    return OnDeviceStateChanged(id, DEVICE_STATE_ACTIVE);
  }
  HRESULT __stdcall OnDeviceRemoved(LPCWSTR id) noexcept override {
    return OnDeviceStateChanged(id, DEVICE_STATE_NOTPRESENT);
  }
  HRESULT __stdcall OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                           LPCWSTR id) noexcept override {
    if (flow == eRender && role == eMultimedia) {
      try {
        EnqueueDeviceEvent(events,
                           {id ? winrt::to_string(id) : "", 0, true, false});
      } catch (...) {
        EnqueueDeviceEvent(events, {});
      }
    }
    return S_OK;
  }
  HRESULT __stdcall OnPropertyValueChanged(LPCWSTR id,
                                           PROPERTYKEY) noexcept override {
    try {
      EnqueueDeviceEvent(events, {winrt::to_string(id), 0, false, true});
    } catch (...) {
      EnqueueDeviceEvent(events, {});
    }
    return S_OK;
  }
};
std::string Identity(IMMDevice *device) {
  LPWSTR id{};
  winrt::check_hresult(device->GetId(&id));
  std::string result;
  try {
    result = winrt::to_string(id);
  } catch (...) {
    CoTaskMemFree(id);
    throw;
  }
  CoTaskMemFree(id);
  return result;
}
}  // namespace
AudioFormat ValidateAudioFormat(WAVEFORMATEX const &format) {
  if (format.nSamplesPerSec < 8000 || format.nSamplesPerSec > 192000 ||
      !format.nChannels || format.nChannels > 8 ||
      (format.wBitsPerSample != 16 && format.wBitsPerSample != 24 &&
       format.wBitsPerSample != 32) ||
      format.nBlockAlign != format.nChannels * (format.wBitsPerSample / 8) ||
      format.nAvgBytesPerSec != format.nSamplesPerSec * format.nBlockAlign)
    throw std::runtime_error(
        "Audio mix rate, channel count, sample container or alignment is "
        "unsupported");
  DWORD mask = format.nChannels == 1 ? SPEAKER_FRONT_CENTER
               : format.nChannels == 2
                   ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                   : 0;
  WORD tag = format.wFormatTag;
  if (tag == WAVE_FORMAT_EXTENSIBLE) {
    if (format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
      throw std::runtime_error("Extensible mix format is truncated");
    auto const &extended =
        reinterpret_cast<WAVEFORMATEXTENSIBLE const &>(format);
    if (extended.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
      tag = WAVE_FORMAT_IEEE_FLOAT;
    else if (extended.SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
      tag = WAVE_FORMAT_PCM;
    else
      throw std::runtime_error("Audio subtype is unsupported");
    if (!extended.Samples.wValidBitsPerSample ||
        extended.Samples.wValidBitsPerSample > format.wBitsPerSample)
      throw std::runtime_error("Valid audio bits exceed their container");
    if (extended.dwChannelMask) mask = extended.dwChannelMask;
  }
  if ((tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT) ||
      (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample != 32) || !mask ||
      std::popcount(mask) != format.nChannels)
    throw std::runtime_error(
        "Audio channel mapping or PCM/float format cannot be interpreted");
  return {format.nSamplesPerSec, format.nChannels, format.wBitsPerSample, mask};
}
DeviceManager::DeviceManager() : DeviceManager(true) {}
DeviceManager::DeviceManager(bool native)
    : events_(std::make_shared<DeviceEventsState>()) {
  if (!native) return;
  winrt::check_hresult(
      CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                       __uuidof(IMMDeviceEnumerator), enumerator_.put_void()));
  notifications_ =
      winrt::make_self<Notifications>(events_).as<IMMNotificationClient>();
  winrt::check_hresult(
      enumerator_->RegisterEndpointNotificationCallback(notifications_.get()));
}
DeviceManager::~DeviceManager() {
  events_->closed = true;
  if (enumerator_ && notifications_)
    enumerator_->UnregisterEndpointNotificationCallback(notifications_.get());
}
std::string DeviceManager::Resolve(AudioSourceConfig const &source) {
  if (source.role != "multimedia" ||
      (!source.follow_system_default && source.endpoint_id.empty()))
    throw std::runtime_error(
        "Capture source requires multimedia role and an explicit specified "
        "endpoint");
  for (auto const &device : Enumerate())
    if (source.follow_system_default ? device.is_default
                                     : device.id == source.endpoint_id)
      return device.id;
  throw std::runtime_error("Selected audio output is unavailable");
}
std::vector<AudioDevice> DeviceManager::Enumerate() {
  std::string default_id;
  winrt::com_ptr<IMMDevice> default_device;
  if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                     default_device.put())))
    default_id = Identity(default_device.get());
  winrt::com_ptr<IMMDeviceCollection> collection;
  winrt::check_hresult(enumerator_->EnumAudioEndpoints(
      eRender, DEVICE_STATE_ACTIVE, collection.put()));
  UINT count{};
  winrt::check_hresult(collection->GetCount(&count));
  std::vector<AudioDevice> result;
  for (UINT i = 0; i < count; ++i) {
    winrt::com_ptr<IMMDevice> device;
    winrt::check_hresult(collection->Item(i, device.put()));
    AudioDevice value;
    value.id = Identity(device.get());
    value.is_default = value.id == default_id;
    winrt::com_ptr<IPropertyStore> properties;
    winrt::check_hresult(
        device->OpenPropertyStore(STGM_READ, properties.put()));
    PROPVARIANT name{};
    winrt::check_hresult(properties->GetValue(PKEY_Device_FriendlyName, &name));
    try {
      if (name.vt != VT_LPWSTR || !name.pwszVal)
        throw std::runtime_error("Audio endpoint friendly name missing");
      value.name = winrt::to_string(name.pwszVal);
    } catch (...) {
      PropVariantClear(&name);
      throw;
    }
    PropVariantClear(&name);
    winrt::com_ptr<IAudioClient> client;
    winrt::check_hresult(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                          nullptr, client.put_void()));
    WAVEFORMATEX *format{};
    winrt::check_hresult(client->GetMixFormat(&format));
    try {
      value.mix_format.resize(sizeof(WAVEFORMATEX) + format->cbSize);
      std::memcpy(value.mix_format.data(), format, value.mix_format.size());
    } catch (...) {
      CoTaskMemFree(format);
      throw;
    }
    CoTaskMemFree(format);
    result.push_back(std::move(value));
  }
  return result;
}
void DeviceManager::Notify(DeviceEvent event) noexcept {
  EnqueueDeviceEvent(events_, std::move(event));
}
std::vector<DeviceEvent> DeviceManager::TakeEvents() {
  std::lock_guard lock(events_->mutex);
  if (events_->failed.exchange(false))
    throw std::runtime_error(
        "Audio endpoint notification copy failed or exceeded its queue bound");
  std::vector<DeviceEvent> result;
  result.swap(events_->events);
  return result;
}
}  // namespace Win2Air
