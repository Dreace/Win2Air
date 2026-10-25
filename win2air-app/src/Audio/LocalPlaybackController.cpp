#include "LocalPlaybackController.h"

#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cmath>

#include "../Settings/StorageIO.h"
#include "../Streaming/FailureDetail.h"
namespace Win2Air {
using namespace winrt::Windows::Data::Json;
namespace {
struct EndpointEvents {
  std::mutex mutex;
  GUID context{};
  bool external{};
};
struct VolumeCallback
    : winrt::implements<VolumeCallback, IAudioEndpointVolumeCallback> {
  std::shared_ptr<EndpointEvents> state;
  explicit VolumeCallback(std::shared_ptr<EndpointEvents> value)
      : state(std::move(value)) {}
  HRESULT __stdcall OnNotify(
      PAUDIO_VOLUME_NOTIFICATION_DATA value) noexcept override {
    if (!value) return E_INVALIDARG;
    std::lock_guard lock(state->mutex);
    if (value->guidEventContext != state->context) state->external = true;
    return S_OK;
  }
};
class NativeEndpoint : public EndpointVolume {
  winrt::com_ptr<IAudioEndpointVolume> value_;
  std::shared_ptr<EndpointEvents> events_ = std::make_shared<EndpointEvents>();
  winrt::com_ptr<IAudioEndpointVolumeCallback> callback_;
  void SetContext(GUID const &context) {
    std::lock_guard lock(events_->mutex);
    events_->context = context;
  }

 public:
  explicit NativeEndpoint(std::string const &endpoint) {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    winrt::check_hresult(
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
    winrt::com_ptr<IMMDevice> device;
    winrt::check_hresult(enumerator->GetDevice(
        winrt::to_hstring(endpoint).c_str(), device.put()));
    winrt::check_hresult(device->Activate(__uuidof(IAudioEndpointVolume),
                                          CLSCTX_ALL, nullptr,
                                          value_.put_void()));
    callback_ = winrt::make_self<VolumeCallback>(events_)
                    .as<IAudioEndpointVolumeCallback>();
    winrt::check_hresult(value_->RegisterControlChangeNotify(callback_.get()));
  }
  ~NativeEndpoint() {
    if (value_ && callback_)
      value_->UnregisterControlChangeNotify(callback_.get());
  }
  EndpointLevel Read() override {
    float volume{};
    BOOL mute{};
    winrt::check_hresult(value_->GetMasterVolumeLevelScalar(&volume));
    winrt::check_hresult(value_->GetMute(&mute));
    return {volume, mute != FALSE};
  }
  void Mute(bool value, GUID const &context) override {
    SetContext(context);
    winrt::check_hresult(value_->SetMute(value, &context));
  }
  void Volume(float value, GUID const &context) override {
    SetContext(context);
    winrt::check_hresult(value_->SetMasterVolumeLevelScalar(value, &context));
  }
  bool ExternalChange() override {
    std::lock_guard lock(events_->mutex);
    bool value = events_->external;
    events_->external = false;
    return value;
  }
};
GUID Context(std::string const &text) {
  GUID value{};
  winrt::check_hresult(
      CLSIDFromString(winrt::to_hstring(text).c_str(), &value));
  return value;
}
std::string Text(GUID const &value) {
  wchar_t text[40]{};
  if (!StringFromGUID2(value, text, 40))
    throw std::runtime_error("Format local playback context failed");
  return winrt::to_string(text);
}
}  // namespace
std::unique_ptr<EndpointVolume> OpenEndpointVolume(
    std::string const &endpoint) {
  return std::make_unique<NativeEndpoint>(endpoint);
}
LocalPlaybackController::LocalPlaybackController(std::filesystem::path root,
                                                 EndpointFactory factory)
    : file_(root / L"local-playback-lease.json"), factory_(std::move(factory)) {
  StorageIO::PrivateDirectory(root);
}
void LocalPlaybackController::Persist() {
  if (!lease_) throw std::runtime_error("Local playback lease absent");
  auto const &value = *lease_;
  JsonObject json;
  json.Insert(L"schema_version", JsonValue::CreateNumberValue(1));
  json.Insert(L"endpoint_id", JsonValue::CreateStringValue(
                                  winrt::to_hstring(value.endpoint_id)));
  json.Insert(L"context", JsonValue::CreateStringValue(
                              winrt::to_hstring(value.app_context_guid)));
  json.Insert(L"epoch", JsonValue::CreateStringValue(winrt::to_hstring(
                            std::to_string(value.lease_epoch))));
  json.Insert(L"volume", JsonValue::CreateNumberValue(value.original_volume));
  json.Insert(L"mute", JsonValue::CreateBooleanValue(value.original_mute));
  json.Insert(L"owned_mute", JsonValue::CreateBooleanValue(value.owned_mute));
  json.Insert(L"external_change",
              JsonValue::CreateBooleanValue(value.external_change));
  json.Insert(L"restore_pending",
              JsonValue::CreateBooleanValue(value.restore_pending));
  StorageIO::AtomicText(file_, winrt::to_string(json.Stringify()));
}
void LocalPlaybackController::Recover() {
  if (lease_) throw std::runtime_error("Recovery requires a fresh controller");
  if (!std::filesystem::exists(file_)) return;
  auto bytes = StorageIO::Read(file_, 16384);
  auto json = JsonObject::Parse(
      winrt::to_hstring(std::string(bytes.begin(), bytes.end())));
  if (json.Size() != 9 || json.GetNamedNumber(L"schema_version") != 1)
    throw std::runtime_error("Unsupported local playback recovery record");
  LocalPlaybackLease value;
  value.endpoint_id = winrt::to_string(json.GetNamedString(L"endpoint_id"));
  value.app_context_guid = winrt::to_string(json.GetNamedString(L"context"));
  Context(value.app_context_guid);
  auto epoch = winrt::to_string(json.GetNamedString(L"epoch"));
  size_t end{};
  value.lease_epoch = std::stoull(epoch, &end);
  value.original_volume = json.GetNamedNumber(L"volume");
  value.original_mute = json.GetNamedBoolean(L"mute");
  value.owned_mute = json.GetNamedBoolean(L"owned_mute");
  value.external_change = json.GetNamedBoolean(L"external_change");
  value.restore_pending = json.GetNamedBoolean(L"restore_pending");
  if (value.endpoint_id.empty() || value.endpoint_id.size() > 4096 ||
      value.endpoint_id.find('\0') != std::string::npos || !value.lease_epoch ||
      end != epoch.size() || epoch[0] == '-' ||
      !std::isfinite(value.original_volume) || value.original_volume < 0 ||
      value.original_volume > 1)
    throw std::runtime_error("Invalid local playback recovery snapshot");
  lease_ = std::move(value);
  Restore();
}
void LocalPlaybackController::Disable(std::string const &endpoint,
                                      uint64_t epoch) {
  if (endpoint.empty() || endpoint.size() > 4096 ||
      endpoint.find('\0') != std::string::npos || !epoch)
    throw std::runtime_error("Mute lease requires explicit endpoint and epoch");
  if (lease_) {
    if (lease_->endpoint_id == endpoint && !lease_->restore_pending) {
      if (lease_->lease_epoch != epoch) {
        lease_->lease_epoch = epoch;
        Persist();
      }
      return;
    }
    Restore();
  }
  auto target = factory_(endpoint);
  auto original = target->Read();
  if (!std::isfinite(original.volume) || original.volume < 0 ||
      original.volume > 1)
    throw std::runtime_error(
        "Endpoint volume snapshot is outside its scalar range");
  GUID context{};
  winrt::check_hresult(CoCreateGuid(&context));
  lease_ =
      LocalPlaybackLease{endpoint,       Text(context), epoch, original.volume,
                         original.muted, true,          false, true};
  try {
    Persist();
  } catch (...) {
    lease_.reset();
    throw;
  }
  target_ = std::move(target);
  try {
    target_->Mute(true, context);
    lease_->restore_pending = false;
    Persist();
  } catch (...) {
    auto original_error = std::current_exception();
    try {
      Restore();
    } catch (...) {
      throw std::runtime_error(
          "Disable local playback failed: " +
          DescribeFailure(original_error).message +
          "; restore failed: " + DescribeFailure().message);
    }
    std::rethrow_exception(original_error);
  }
}
void LocalPlaybackController::Restore() {
  if (!lease_) return;
  lease_->restore_pending = true;
  if (!target_) target_ = factory_(lease_->endpoint_id);
  auto context = Context(lease_->app_context_guid);
  target_->Volume(static_cast<float>(lease_->original_volume), context);
  target_->Mute(lease_->original_mute, context);
  StorageIO::Check(DeleteFileW(file_.c_str()),
                   "Remove restored playback lease");
  lease_.reset();
  target_.reset();
}
void LocalPlaybackController::ObserveExternalChange() {
  if (lease_) {
    lease_->external_change = true;
    Persist();
  }
}
void LocalPlaybackController::PollChanges() {
  if (lease_ && target_ && target_->ExternalChange()) ObserveExternalChange();
}
}  // namespace Win2Air
