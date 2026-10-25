#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <atomic>
#include <cmath>
#include <limits>
#include "../../src/Settings/Appearance.h"
#include "../../src/Settings/StorageIO.h"
#include "../../src/UI/ViewModels/SettingsViewModel.h"
#include "../../src/UI/ViewModels/StreamingViewModel.h"
#include "../contract/test_support.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"
using namespace Win2Air;
using namespace Win2Air::Tests;
void TestSettingsRoundtrip(wchar_t const* directory) {
  auto root = std::filesystem::path(directory);
  SettingsStore store(root / L"roundtrip");
  auto input = store.Load();
  Require(input.first_run && input.settings.language == "system" &&
              input.settings.theme == "system",
          "Initial appearance follows the system");
  auto& s = input.settings;
  s.revision = 1;
  s.audio_source = {false, "virtual-output", "multimedia"};
  s.selected_receiver_id = "AA:BB:CC:DD:EE:FF";
  s.local_playback = false;
  s.auto_reconnect = false;
  s.receiver_media_control = false;
  s.log_level = "warning";
  s.latency_mode = "custom";
  s.custom_ms = 1250;
  s.silence_timeout = 60;
  s.standby_disconnect_seconds = 300;
  s.startup = s.minimized = s.restore_previous = true;
  s.close_to_tray = false;
  s.theme = "dark";
  s.language = "en-US";
  s.statistics = {{1, 600}, {0, 3600}};
  s.window_placement = {-5000, 5000, 1200, 900, true};
  ReceiverRecord r;
  r.receiver_id = s.selected_receiver_id;
  r.identity_verified = true;
  r.name = "Saved receiver";
  r.model = "Virtual model";
  r.favorite = r.hidden = r.auto_connect = r.muted = r.connect_established =
      true;
  r.volume = 0.8;
  r.offset_ms = -100;
  r.features = UINT64_MAX;
  r.firmware_version = "virtual-firmware";
  r.latency_profile =
      LatencyProfile{"virtual-firmware", "127.0.0.1:7100|if=17|scope=4", 150};
  r.manual_endpoint =
      ReceiverEndpoint{"virtual.local", "127.0.0.1", 7100, 17, 4, 29, 31};
  r.discovered_endpoints = {*r.manual_endpoint};
  input.receivers = {r};
  store.Save(input);
  auto output = SettingsStore(root / L"roundtrip").Load();
  auto const& a = output.settings;
  auto const& b = output.receivers.front();
  Require(!output.first_run && a.schema_version == 1 && a.revision == 1 &&
              a.audio_source.follow_system_default ==
                  s.audio_source.follow_system_default &&
              a.audio_source.endpoint_id == s.audio_source.endpoint_id &&
              a.audio_source.role == s.audio_source.role &&
              a.selected_receiver_id == s.selected_receiver_id &&
              a.local_playback == s.local_playback &&
              a.auto_reconnect == s.auto_reconnect &&
              a.receiver_media_control == s.receiver_media_control &&
              a.log_level == s.log_level && a.latency_mode == s.latency_mode &&
              a.custom_ms == s.custom_ms &&
              a.silence_timeout == s.silence_timeout &&
              a.standby_disconnect_seconds == s.standby_disconnect_seconds &&
              a.startup == s.startup && a.minimized == s.minimized &&
              a.restore_previous == s.restore_previous &&
              a.close_to_tray == s.close_to_tray && a.theme == s.theme &&
              a.language == s.language && a.statistics == s.statistics &&
              a.window_placement.x == s.window_placement.x &&
              a.window_placement.y == s.window_placement.y &&
              a.window_placement.width == s.window_placement.width &&
              a.window_placement.height == s.window_placement.height &&
              a.window_placement.saved,
          "All application preference fields survive a new store instance");
  auto invalid_chart = output;
  invalid_chart.settings.statistics.primary.seconds = 120;
  Rejects([&] { ValidateSettings(invalid_chart); },
          "Statistics preferences reject unsupported time ranges");
  invalid_chart = output;
  invalid_chart.settings.statistics.secondary.metric = 5;
  Rejects([&] { ValidateSettings(invalid_chart); },
          "Statistics preferences reject unknown metrics");
  Require(
      b.receiver_id == r.receiver_id && b.identity_verified &&
          b.name == r.name && b.model == r.model && b.favorite && b.hidden &&
          b.auto_connect && b.muted && b.connect_established &&
          b.volume == r.volume && b.offset_ms == r.offset_ms &&
          b.features == r.features &&
          b.firmware_version == r.firmware_version && b.latency_profile &&
          b.latency_profile->budget_ms == 150 &&
          b.latency_profile->firmware == r.latency_profile->firmware &&
          b.latency_profile->network_context ==
              r.latency_profile->network_context &&
          b.manual_endpoint && b.discovered_endpoints.size() == 1,
      "All receiver preferences and exact feature bits survive persistence");
  for (auto const& e : {*b.manual_endpoint, b.discovered_endpoints.front()})
    Require(e.host == r.manual_endpoint->host &&
                e.resolved_address == r.manual_endpoint->resolved_address &&
                e.port == r.manual_endpoint->port && e.interface_index == 17 &&
                e.scope_id == 4 && e.discovery_epoch == 29 && e.last_seen == 31,
            "Endpoint identity and observation fields survive persistence");
  {
    using namespace winrt::Windows::Data::Json;
    auto metadata_root = root / L"optional-device-metadata";
    SettingsStore metadata_store(metadata_root);
    metadata_store.Save(input);
    auto path = metadata_root / L"receivers.json";
    auto bytes = StorageIO::Read(path);
    auto document = JsonObject::Parse(
        winrt::to_hstring(std::string(bytes.begin(), bytes.end())));
    auto record = document.GetNamedArray(L"records").GetObjectAt(0);
    record.Remove(L"firmware_version");
    record.Remove(L"latency_profile");
    StorageIO::AtomicText(path, winrt::to_string(document.Stringify()));
    auto unknown_metadata = metadata_store.Load().receivers.front();
    Require(unknown_metadata.firmware_version.empty() &&
                !unknown_metadata.latency_profile &&
                unknown_metadata.receiver_id == r.receiver_id,
            "A device without observed firmware or a learned budget keeps its "
            "public identity and preferences");
    record.Insert(L"firmware_version", JsonValue::CreateNumberValue(7));
    StorageIO::AtomicText(path, winrt::to_string(document.Stringify()));
    Rejects([&] { metadata_store.Load(); },
            "Present firmware metadata must have its declared string type");
    record.Remove(L"firmware_version");
    record.Insert(L"unknown_field", JsonValue::CreateBooleanValue(true));
    StorageIO::AtomicText(path, winrt::to_string(document.Stringify()));
    Rejects([&] { metadata_store.Load(); },
            "Optional metadata does not allow arbitrary configuration fields");
  }
  {
    StorageIO::Handle locked(
        CreateFileW((root / L"roundtrip" / L"commit.json").c_str(),
                    GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
    auto candidate = input;
    candidate.settings.revision = 2;
    Rejects([&] { store.Save(candidate); },
            "A late commit failure remains a failed transaction");
    Rejects([&] { store.Load(); },
            "Mismatched revisions never restore a guessed configuration");
  }
  input.settings.revision = 2;
  store.Save(input);
  auto invalid = input;
  invalid.settings.revision = 3;
  invalid.settings.language = "unexpected";
  Rejects([&] { store.Save(invalid); }, "Unsupported languages are rejected");
  invalid = input;
  invalid.settings.revision = 3;
  invalid.settings.schema_version = 99;
  Rejects([&] { store.Save(invalid); },
          "Unknown schemas are rejected without migration");
  Require(store.Load().settings.revision == 2,
          "Rejected values retain the last committed settings");
  auto service_root = root / L"service";
  VirtualReceiver receiver;
  auto audio = std::make_shared<VirtualAudioState>();
  auto services = VirtualAudioServices(audio);
  std::atomic<LogLevel> level{LogLevel::trace};
  services.log_level_changed = [&](LogLevel value) { level = value; };
  StreamCoordinator service(receiver, service_root, false, services);
  Require(level == DefaultLogLevel(),
          "An unconfigured service uses this build's logging default");
  Require(service.SetLogLevel("error").get().status == CommandStatus::applied &&
              level == LogLevel::error &&
              SettingsStore(service_root).Load().settings.log_level == "error",
          "Log level changes reach the live sink and persist");
  Require(
      service.SetLogLevel("invalid").get().status == CommandStatus::failed &&
          level == LogLevel::error &&
          SettingsStore(service_root).Load().settings.log_level == "error",
      "An invalid log level preserves the current filter and saved preference");
  SettingsViewModel model(service);
  model.Logging("debug");
  WaitUntil(
      [&] {
        model.Refresh();
        return !model.Pending();
      },
      "Log level selection did not complete");
  Require(
      level == LogLevel::debug &&
          model.Snapshot().preferences.log_level == "debug",
      "The settings view model changes logging without restarting the session");
  model.Refresh();
  model.Draft().theme.Edit("dark");
  auto streaming_draft = std::make_shared<StreamingDraft>();
  StreamingViewModel streaming_model(service, streaming_draft);
  streaming_model.Refresh();
  streaming_model.Draft().mode.Edit("custom");
  streaming_model.Draft().custom.Edit(420);
  Require(service.SetSilenceTimeout(60).get().status == CommandStatus::applied,
          "Unrelated immediate settings save completes");
  model.Refresh();
  streaming_model.Refresh();
  Require(model.Draft().theme.Value() == "dark" &&
              streaming_model.Draft().mode.Value() == "custom" &&
              streaming_model.Draft().custom.Value() == 420,
          "Immediate saves retain unapplied appearance and latency drafts");
  StreamingViewModel recreated(service, streaming_draft);
  recreated.Refresh();
  Require(recreated.Draft().custom.Value() == 420,
          "Recreating a localized view retains the shared draft");
  streaming_model.Latency("custom", 420);
  WaitUntil(
      [&] {
        streaming_model.Refresh();
        return !streaming_model.Snapshot().pending;
      },
      "Draft budget was not applied");
  Require(!streaming_model.Draft().custom.Dirty() &&
              streaming_model.Snapshot().playback.preferences.custom_ms == 420,
          "Only an acknowledged matching save clears a draft");
  Require(streaming_model.OffsetRange() == std::pair{-400, 1000},
          "Offset guidance follows the applied budget");
  Rejects(
      [&] { streaming_model.Offset(-401); },
      "An offset outside the combined budget is rejected before submission");
  streaming_model.Draft().custom.Edit(500);
  streaming_model.Latency("custom", 430);
  WaitUntil(
      [&] {
        streaming_model.Refresh();
        return !streaming_model.Snapshot().pending;
      },
      "In-flight save did not finish");
  Require(streaming_model.Draft().custom.Value() == 500 &&
              streaming_model.Draft().custom.Dirty(),
          "An acknowledgement cannot overwrite a newer edit");
  streaming_model.Draft().custom.Edit(450);
  streaming_model.Latency("custom", 450);
  streaming_model.Draft().custom.Edit(430);
  WaitUntil(
      [&] {
        streaming_model.Refresh();
        return !streaming_model.Snapshot().pending;
      },
      "Reverted edit save did not finish");
  Require(streaming_model.Draft().custom.Value() == 430 &&
              streaming_model.Draft().custom.Dirty(),
          "Editing back to the previous saved value remains a newer edit");
  streaming_model.Draft().custom.Edit(std::numeric_limits<double>::quiet_NaN());
  streaming_model.Draft().mode.Edit("balanced");
  streaming_model.Latency(
      "balanced", streaming_model.Snapshot().playback.preferences.custom_ms);
  WaitUntil(
      [&] {
        streaming_model.Refresh();
        return !streaming_model.Snapshot().pending;
      },
      "Preset recovery did not finish");
  Require(streaming_model.Snapshot().playback.preferences.latency_mode ==
                  "balanced" &&
              std::isnan(streaming_model.Draft().custom.Value()),
          "A preset saves without consuming an invalid hidden custom draft");
  model.Draft().source.Edit("virtual-specified");
  model.Source({false, "virtual-specified", "multimedia"});
  WaitUntil(
      [&] {
        model.Refresh();
        return !model.Pending();
      },
      "Selected audio source did not save");
  auto selected_source =
      SettingsStore(service_root).Load().settings.audio_source;
  Require(model.Result().status == CommandStatus::applied &&
              !selected_source.follow_system_default &&
              selected_source.endpoint_id == "virtual-specified" &&
              !model.Draft().source.Dirty(),
          "A selected virtual audio source persists without a separate commit");
  model.Draft().source.Edit("");
  model.Source({true, "", "multimedia"});
  WaitUntil(
      [&] {
        model.Refresh();
        return !model.Pending();
      },
      "Following the default audio source did not save");
  Require(SettingsStore(service_root)
              .Load()
              .settings.audio_source.follow_system_default,
          "Choosing the default source restores system-following behavior");
  model.Appearance("light", "zh-CN");
  Require(model.Pending(),
          "Submission remains pending until the save result is consumed");
  WaitUntil(
      [&] {
        model.Refresh();
        return !model.Pending();
      },
      "Appearance save did not finish");
  Require(model.Result().status == CommandStatus::applied &&
              SettingsStore(service_root).Load().settings.theme == "light",
          "Saved appearance is confirmed by persistence");
  {
    StorageIO::Handle locked(
        CreateFileW((service_root / L"settings.json").c_str(), GENERIC_READ,
                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr));
    model.Draft().theme.Edit("dark");
    model.Draft().language.Edit("en-US");
    model.Appearance("dark", "en-US");
    WaitUntil(
        [&] {
          model.Refresh();
          return !model.Pending();
        },
        "Failed save did not finish");
    Require(model.Result().status == CommandStatus::failed &&
                !model.Result().message.empty() &&
                model.Snapshot().preferences.theme == "light" &&
                model.Draft().theme.Value() == "light" &&
                model.Draft().language.Value() == "zh-CN" &&
                !model.Draft().theme.Dirty(),
            "Failed immediate selections restore saved choices and report "
            "the save failure");
    model.Draft().source.Edit("virtual-specified");
    model.Source({false, "virtual-specified", "multimedia"});
    WaitUntil(
        [&] {
          model.Refresh();
          return !model.Pending();
        },
        "Failed source selection did not finish");
    Require(model.Result().status == CommandStatus::failed &&
                model.Draft().source.Value().empty() &&
                !model.Draft().source.Dirty(),
            "A failed source selection restores the saved default source");
    streaming_model.Draft().mode.Edit("stable");
    streaming_model.Latency("stable", 450);
    WaitUntil(
        [&] {
          streaming_model.Refresh();
          return !streaming_model.Snapshot().pending;
        },
        "Failed preset selection did not finish");
    Require(!streaming_model.Snapshot().error.empty() &&
                streaming_model.Draft().mode.Value() == "balanced" &&
                !streaming_model.Draft().mode.Dirty(),
            "A failed latency preset restores the saved mode and reports "
            "the failure");
  }
  SettingDraft<std::string> newer_choice;
  newer_choice.Update("system");
  newer_choice.Edit("light");
  auto rejected_version = newer_choice.Version();
  newer_choice.Edit("dark");
  newer_choice.Reject(rejected_version, "system");
  Require(newer_choice.Value() == "dark" && newer_choice.Dirty(),
          "Rejecting an earlier save preserves the newer selection");
  auto closed = service.Shutdown(true);
  Require(closed.get().status == CommandStatus::applied,
          "Settings service closes without a receiver");
}
