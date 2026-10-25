#include "../../src/Audio/LocalPlaybackController.h"
#include "test_support.h"
using namespace Win2Air;
namespace {
struct State {
  EndpointLevel level{0.65f, false};
  unsigned mute_writes{}, volume_writes{};
  bool fail{}, fail_after_mute{};
};
class FakeEndpoint : public EndpointVolume {
  State& state_;

 public:
  explicit FakeEndpoint(State& state) : state_(state) {}
  EndpointLevel Read() override { return state_.level; }
  void Mute(bool value, GUID const&) override {
    if (state_.fail) throw std::runtime_error("Endpoint unavailable");
    ++state_.mute_writes;
    state_.level.muted = value;
    if (state_.fail_after_mute) {
      state_.fail_after_mute = false;
      throw std::runtime_error("Mute write completed before reported failure");
    }
  }
  void Volume(float value, GUID const&) override {
    if (state_.fail) throw std::runtime_error("Endpoint unavailable");
    ++state_.volume_writes;
    state_.level.volume = value;
  }
};
}  // namespace
void TestLocalPlaybackLease(wchar_t const* root) {
  State state;
  auto factory = [&](std::string const& id) {
    Require(id == "test-render", "Lease binds the selected endpoint");
    return std::make_unique<FakeEndpoint>(state);
  };
  LocalPlaybackController control(root, factory);
  control.Recover();
  control.Restore();
  Require(!state.mute_writes && !state.volume_writes,
          "Default local playback does not change the endpoint");
  state.fail_after_mute = true;
  Rejects([&] { control.Disable("test-render", 1); },
          "Failed mute acquisition is reported after rollback");
  Require(!state.level.muted && state.level.volume == 0.65f && !control.Lease(),
          "Failed acquisition restores the original endpoint before returning");
  state.mute_writes = 0;
  state.volume_writes = 0;
  control.Disable("test-render", 1);
  auto lease_file = std::filesystem::path(root) / L"local-playback-lease.json";
  Require(SetFileAttributesW(lease_file.c_str(), FILE_ATTRIBUTE_READONLY),
          "Block recovery record writes and deletion");
  Rejects([&] { control.Restore(); },
          "Recovery record deletion failure is reported");
  Require(!state.level.muted && state.level.volume == 0.65f &&
              control.Lease()->restore_pending,
          "Endpoint restoration still runs when recovery storage cannot be "
          "updated");
  Require(SetFileAttributesW(lease_file.c_str(), FILE_ATTRIBUTE_NORMAL),
          "Release recovery record storage failure");
  control.Restore();
  state.mute_writes = 0;
  state.volume_writes = 0;
  control.Disable("test-render", 1);
  Require(state.level.muted && state.level.volume == 0.65f,
          "Disable uses endpoint mute while preserving volume");
  control.Disable("test-render", 1);
  Require(state.mute_writes == 1,
          "Repeated disable does not replace the original snapshot");
  state.level = {0.2f, false};
  control.ObserveExternalChange();
  control.Restore();
  Require(state.level.volume == 0.65f && !state.level.muted && !control.Lease(),
          "Restore reinstates original snapshot after external changes");
  control.Disable("test-render", 2);
  state.fail = true;
  Rejects([&] { control.Restore(); }, "Restore errors are surfaced");
  Require(control.Lease()->restore_pending,
          "Restore failure retains the lease");
  state.fail = false;
  LocalPlaybackController recovered(root, factory);
  recovered.Recover();
  Require(!state.level.muted && state.level.volume == 0.65f,
          "Fresh controller restores the persisted crash snapshot");
  state.level.muted = true;
  recovered.Disable("test-render", 3);
  recovered.Restore();
  Require(state.level.muted, "Original mute is preserved");
}
