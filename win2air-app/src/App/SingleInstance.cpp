#include "SingleInstance.h"
#include <stdexcept>
namespace Win2Air {
std::wstring InstanceKey(std::wstring const& user_sid,
                         std::wstring const& identity) {
  if (!user_sid.starts_with(L"S-1-") || user_sid.size() > 256 ||
      identity.empty() || identity.size() > 256 ||
      user_sid.find_first_not_of(L"S0123456789-") != std::wstring::npos ||
      identity.find_first_of(L"\\/\r\n") != std::wstring::npos ||
      identity.find(L'\0') != std::wstring::npos)
    throw std::runtime_error(
        "Instance key requires a user SID and application identity");
  return L"Win2Air:" + user_sid + L":" + identity;
}
bool ClaimInstance(InstanceBackend& backend, std::wstring const& user_sid,
                   std::wstring const& identity,
                   ActivationRequest const& request) {
  if (backend.Claim(InstanceKey(user_sid, identity))) return true;
  backend.Redirect(request);
  return false;
}
}  // namespace Win2Air

#ifdef WIN2AIR_DESKTOP
#include <windows.h>
#undef GetCurrentTime

#include <appmodel.h>
#include <sddl.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Foundation.h>
namespace Win2Air {
namespace {
std::wstring UserSid() {
  winrt::handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()))
    winrt::throw_last_error();
  DWORD size{};
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  std::vector<uint8_t> bytes(size);
  if (!size ||
      !GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size))
    winrt::throw_last_error();
  wchar_t* sid{};
  if (!ConvertSidToStringSidW(
          reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
    winrt::throw_last_error();
  std::unique_ptr<void, decltype(&LocalFree)> holder(sid, LocalFree);
  return sid;
}
std::wstring PackageIdentity() {
  UINT32 size{};
  auto status = GetCurrentPackageFamilyName(&size, nullptr);
  if (status == APPMODEL_ERROR_NO_PACKAGE) return L"unpackaged.Win2Air";
  if (status != ERROR_INSUFFICIENT_BUFFER)
    winrt::throw_hresult(HRESULT_FROM_WIN32(status));
  std::vector<wchar_t> name(size);
  status = GetCurrentPackageFamilyName(&size, name.data());
  if (status) winrt::throw_hresult(HRESULT_FROM_WIN32(status));
  return name.data();
}
ActivationRequest Request(
    winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments const&
        args) {
  using Kind = winrt::Microsoft::Windows::AppLifecycle::ExtendedActivationKind;
  using namespace winrt::Windows::ApplicationModel::Activation;
  if (args.Kind() == Kind::CommandLineLaunch)
    return ParseActivation(args.Data()
                               .as<ICommandLineActivatedEventArgs>()
                               .Operation()
                               .Arguments()
                               .c_str());
  if (args.Kind() == Kind::Protocol)
    return ParseProtocol(args.Data()
                             .as<IProtocolActivatedEventArgs>()
                             .Uri()
                             .AbsoluteUri()
                             .c_str());
  if (args.Kind() == Kind::StartupTask) {
    if (args.Data().as<IStartupTaskActivatedEventArgs>().TaskId() !=
        L"Win2AirStartup")
      throw std::runtime_error(
          "Startup activation task identity differs from the application");
    ActivationRequest request;
    request.startup = true;
    return request;
  }
  if (args.Kind() != Kind::Launch)
    throw std::runtime_error("Unsupported application activation kind");
  wchar_t executable[32768]{};
  auto size = GetModuleFileNameW(nullptr, executable, 32768);
  if (!size || size >= 32768) winrt::throw_last_error();
  auto text = args.Data().as<ILaunchActivatedEventArgs>().Arguments();
  return ParseActivation(LaunchArguments(text.c_str(), executable));
}
}  // namespace
struct NativeInstance::Impl : InstanceBackend,
                              std::enable_shared_from_this<Impl> {
  winrt::Microsoft::Windows::AppLifecycle::AppInstance current{nullptr},
      owner{nullptr};
  winrt::Microsoft::Windows::AppLifecycle::AppActivationArguments arguments{
      nullptr};
  winrt::event_token token{};
  std::shared_ptr<ActivationQueue> queue = std::make_shared<ActivationQueue>();
  std::mutex mutex;
  std::string error;
  ActivationRequest initial;
  bool primary{}, subscribed{};
  bool Claim(std::wstring const& key) override {
    owner = winrt::Microsoft::Windows::AppLifecycle::AppInstance::
        FindOrRegisterForKey(key);
    if (!owner)
      throw std::runtime_error(
          "Registered primary instance became unavailable");
    return owner.IsCurrent();
  }
  void Redirect(ActivationRequest const&) override {
    auto operation = owner.RedirectActivationToAsync(arguments);
    if (operation.wait_for(std::chrono::seconds(3)) ==
        winrt::Windows::Foundation::AsyncStatus::Started) {
      operation.Cancel();
      throw std::runtime_error(
          "Primary activation redirection exceeded its deadline");
    }
    operation.GetResults();
  }
  void Start() {
    current =
        winrt::Microsoft::Windows::AppLifecycle::AppInstance::GetCurrent();
    arguments = current.GetActivatedEventArgs();
    initial = Request(arguments);
    auto weak = weak_from_this();
    token = current.Activated([weak](auto const&, auto const& args) noexcept {
      if (auto self = weak.lock()) {
        try {
          self->queue->Submit(Request(args));
        } catch (winrt::hresult_error const& failure) {
          std::lock_guard lock(self->mutex);
          self->error = winrt::to_string(failure.message()) +
                        "; HRESULT=" + std::to_string(failure.code().value);
        } catch (std::exception const& failure) {
          std::lock_guard lock(self->mutex);
          self->error = failure.what();
        } catch (...) {
          std::lock_guard lock(self->mutex);
          self->error = "Activation redirection failed before command dispatch";
        }
      }
    });
    subscribed = true;
    primary = ClaimInstance(*this, UserSid(), PackageIdentity(), initial);
  }
  ~Impl() {
    if (subscribed) current.Activated(token);
    try {
      if (primary && owner) owner.UnregisterKey();
    } catch (...) {
      OutputDebugStringW(L"Unregister application instance failed during exit");
    }
  }
};
NativeInstance::NativeInstance() : impl_(std::make_shared<Impl>()) {
  impl_->Start();
}
NativeInstance::~NativeInstance() = default;
bool NativeInstance::Primary() const { return impl_->primary; }
ActivationRequest NativeInstance::Initial() const { return impl_->initial; }
std::shared_ptr<ActivationQueue> NativeInstance::Queue() const {
  return impl_->queue;
}
std::string NativeInstance::TakeError() {
  std::lock_guard lock(impl_->mutex);
  return std::exchange(impl_->error, {});
}
}  // namespace Win2Air
#endif
