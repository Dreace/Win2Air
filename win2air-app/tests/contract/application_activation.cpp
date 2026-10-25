#include "../../src/App/SingleInstance.h"
#include "../../src/UI/ViewModels/NavigationViewModel.h"
#include "test_support.h"
using namespace Win2Air;
namespace {
struct Instance final : InstanceBackend {
  bool primary{true}, fail_redirect{};
  unsigned claims{}, redirects{};
  ActivationQueue& queue;
  explicit Instance(ActivationQueue& target) : queue(target) {}
  bool Claim(std::wstring const& key) override {
    Require(key.find(L"S-1-5-21-7") != std::wstring::npos,
            "Instance binding retains current user");
    ++claims;
    return primary;
  }
  void Redirect(ActivationRequest const& request) override {
    ++redirects;
    if (fail_redirect)
      throw std::runtime_error("Controlled redirection failed");
    queue.Submit(request);
  }
};
}  // namespace
void TestApplicationActivation() {
  Require(ParseActivation(L"").page == AppPage::playback,
          "Ordinary launch opens playback");
  auto request = ParseActivation(
      L"--open settings --connect AA:BB:CC:DD:EE:FF --minimized");
  Require(request.page == AppPage::settings && request.minimized &&
              request.receiver_id,
          "Explicit activation carries page and single receiver intent");
  Require(
      ParseProtocol(L"win2air://open/statistics").page == AppPage::statistics,
      "Protocol activation resolves only the supported page");
  Require(ParseActivation(L"--open devices").page == AppPage::devices,
          "Devices is an activation page");
  NavigationViewModel navigation;
  navigation.Navigate(AppPage::devices);
  Require(navigation.Page() == AppPage::devices,
          "Devices is a navigation page");
  for (auto text :
       {L"--open", L"--open invalid", L"--open playback --open settings",
        L"--minimized --minimized", L"--connect invalid",
        L"--connect AA:BB:CC:DD:EE:FF --disconnect", L"--unknown",
        L"win2air://open/playback?secret=data", L"win2air://other/playback",
        L"win2air://open/settings#fragment",
        L"win2air://open/settings --disconnect"})
    Rejects(
        [&] { ParseActivation(text); },
        "Unsupported or conflicting activation is rejected before dispatch");
  Require(
      LaunchArguments(
          L"\"C:\\Program Files\\Win2Air\\Win2Air.exe\" --open settings",
          L"C:\\Program Files\\Win2Air\\Win2Air.exe") == L" --open settings",
      "Unpackaged lifecycle arguments safely remove the trusted executable");
  Require(LaunchArguments(L"--open playback", L"C:\\Win2Air.exe") ==
              L"--open playback",
          "Packaged arguments remain intact");
  Require(InstanceKey(L"S-1-5-21-7", L"Win2Air.Desktop") !=
                  InstanceKey(L"S-1-5-21-8", L"Win2Air.Desktop") &&
              InstanceKey(L"S-1-5-21-7", L"Win2Air.Desktop") !=
                  InstanceKey(L"S-1-5-21-7", L"unpackaged.Win2Air"),
          "Instance keys isolate user and application identities");
  Rejects([&] { InstanceKey(L"", L"Win2Air.Desktop"); },
          "Anonymous instance ownership is rejected");
  ActivationQueue queue;
  Instance backend(queue);
  Require(ClaimInstance(backend, L"S-1-5-21-7", L"Win2Air.Desktop", request) &&
              backend.redirects == 0,
          "Primary alone initializes application services");
  backend.primary = false;
  Require(!ClaimInstance(backend, L"S-1-5-21-7", L"Win2Air.Desktop", request) &&
              backend.redirects == 1,
          "Secondary forwards without creating another service");
  auto delivered = queue.Take();
  Require(delivered.size() == 1 &&
              delivered[0].receiver_id == request.receiver_id &&
              queue.Take().empty(),
          "Redirected activation is owned and consumed exactly once");
  backend.fail_redirect = true;
  Rejects(
      [&] {
        ClaimInstance(backend, L"S-1-5-21-7", L"Win2Air.Desktop", request);
      },
      "Redirection failure cannot start another primary");
  queue.Close();
  Rejects([&] { queue.Submit(request); }, "Shutdown rejects late activations");
}
