#pragma once
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <winrt/base.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "../Settings/AppSettings.h"
#include "../Streaming/SessionModels.h"
namespace Win2Air {
struct AudioDevice {
  std::string id, name;
  bool is_default{};
  std::vector<uint8_t> mix_format;
};
struct DeviceEvent {
  std::string endpoint_id;
  DWORD state{};
  bool default_changed{}, format_changed{};
};
struct DeviceEventsState;
class DeviceManager {
  winrt::com_ptr<IMMDeviceEnumerator> enumerator_;
  winrt::com_ptr<IMMNotificationClient> notifications_;
  std::shared_ptr<DeviceEventsState> events_;

 protected:
  explicit DeviceManager(bool native);

 public:
  DeviceManager();
  virtual ~DeviceManager();
  virtual std::vector<AudioDevice> Enumerate();
  std::string Resolve(AudioSourceConfig const& source);
  std::vector<DeviceEvent> TakeEvents();
  void Notify(DeviceEvent event) noexcept;
};
AudioFormat ValidateAudioFormat(WAVEFORMATEX const& format);
}  // namespace Win2Air
