#include <windows.h>
#undef GetCurrentTime
#include <microsoft.ui.xaml.window.h>
#include <psapi.h>
#include <shellapi.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.Data.Json.h>
#include <algorithm>
#include <atomic>
#include "../../src/Settings/StorageIO.h"
#include "../../src/UI/PlaybackPage.h"
#include "../../src/UI/StatisticsPage.h"
#include "../fixtures/AudioFixture.h"
#include "../fixtures/ReceiverFixture.h"

using namespace Win2Air;
using namespace Win2Air::Tests;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Windows::Data::Json;
namespace {
int test_exit_code{};
std::filesystem::path report;
struct MemoryApplication
    : ApplicationT<MemoryApplication, Markup::IXamlMetadataProvider> {
  XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
  Markup::IXamlMetadataProvider chart_metadata{nullptr};
  Markup::IXamlType GetXamlType(
      winrt::Windows::UI::Xaml::Interop::TypeName const& type) {
    if (auto value = metadata.GetXamlType(type)) return value;
    return Charts().GetXamlType(type);
  }
  Markup::IXamlType GetXamlType(winrt::hstring const& name) {
    if (auto value = metadata.GetXamlType(name)) return value;
    return Charts().GetXamlType(name);
  }
  auto GetXmlnsDefinitions() { return metadata.GetXmlnsDefinitions(); }
  Markup::IXamlMetadataProvider Charts() {
    if (!chart_metadata)
      chart_metadata = winrt::get_activation_factory<
                           winrt::Windows::Foundation::IActivationFactory>(
                           L"Win2Air.Charts.Win2Air_Charts_XamlTypeInfo."
                           L"XamlMetaDataProvider")
                           .ActivateInstance<Markup::IXamlMetadataProvider>();
    return chart_metadata;
  }
  Window window{nullptr};
  Grid host;
  DispatcherTimer timer;
  VirtualReceiver receiver;
  std::shared_ptr<VirtualAudioState> audio =
      std::make_shared<VirtualAudioState>();
  std::unique_ptr<StreamCoordinator> service;
  std::unique_ptr<PlaybackPage> playback;
  std::unique_ptr<StatisticsPage> statistics;
  std::atomic<uint64_t> clock{1000000000};
  unsigned tick{};
  uint64_t baseline{};
  JsonArray samples;
  std::vector<winrt::weak_ref<TextBlock>> old_details;
  MemoryApplication() {
    UnhandledException([this](auto const&, auto const& error) {
      Finish(winrt::to_string(error.Message()));
    });
  }
  void OnLaunched(LaunchActivatedEventArgs const&) {
    try {
      Resources().MergedDictionaries().Append(XamlControlsResources{});
      resource_language = "zh-CN";
      auto data = report.parent_path() / report.stem();
      SettingsStore store(data);
      auto saved = store.Load();
      saved.settings.revision++;
      saved.settings.latency_mode = "balanced";
      saved.settings.silence_timeout = 0;
      ReceiverRecord device;
      device.receiver_id = "AA:BB:CC:DD:EE:FF";
      device.name = "\xe5\xae\xa2\xe5\x8e\x85";
      device.model = "HomePod mini";
      device.features =
          (uint64_t(1) << 9) | (uint64_t(1) << 41) | (uint64_t(1) << 48);
      device.manual_endpoint = ReceiverEndpoint{"127.0.0.1", "127.0.0.1", 7000};
      saved.receivers = {device};
      store.Save(saved);
      auto services = VirtualAudioServices(audio);
      services.metrics_now = [this] { return clock.load(); };
      service =
          std::make_unique<StreamCoordinator>(receiver, data, false, services);
      if (service->Connect(device.receiver_id).get().status !=
          CommandStatus::applied)
        throw std::runtime_error("Virtual receiver connection failed");
      window = Window();
      HWND owner{};
      winrt::check_hresult(
          window.as<IWindowNative>()->get_WindowHandle(&owner));
      wchar_t executable[32768]{};
      winrt::check_bool(GetModuleFileNameW(nullptr, executable, 32768));
      auto runtime = std::filesystem::path(executable).parent_path();
      auto theme = StorageIO::Read(runtime / L"ThemeResources.xaml", 65536);
      Resources().MergedDictionaries().Append(
          Markup::XamlReader::Load(
              winrt::to_hstring(std::string(theme.begin(), theme.end())))
              .as<ResourceDictionary>());
      playback = std::make_unique<PlaybackPage>(
          *service, runtime / L"PlaybackPage.xaml", window.DispatcherQueue());
      statistics = std::make_unique<StatisticsPage>(
          *service, runtime / L"StatisticsPage.xaml", owner);
      host.Children().Append(playback->Content());
      host.Children().Append(statistics->Content());
      window.Content(host);
      window.Activate();
      timer.Interval(std::chrono::milliseconds(50));
      timer.Tick([this](auto const&, auto const&) { Tick(); });
      timer.Start();
    } catch (std::exception const& error) {
      Finish(error.what());
    } catch (winrt::hresult_error const& error) {
      Finish(winrt::to_string(error.message()));
    }
  }
  void Tick() {
    try {
      bool visible = tick % 2 == 0;
      if (tick % 300 == 0) {
        for (auto name : {L"HistoryWindow", L"AudioHistoryWindow"})
          statistics->Content().FindName(name).as<ComboBox>().SelectedIndex(
              (tick / 300) % 2);
      }
      playback->Content().Visibility(visible ? Visibility::Collapsed
                                             : Visibility::Visible);
      statistics->Content().Visibility(visible ? Visibility::Visible
                                               : Visibility::Collapsed);
      auto packet = NonzeroPacket(2205);
      packet.position = uint64_t(tick) * 2205;
      audio->Push(std::move(packet));
      clock += 1000000000;
      playback->Refresh();
      statistics->Refresh(visible);
      if (++tick % 100) return;
      auto detail_rows =
          statistics->Content().FindName(L"MetricDetailsRows").as<StackPanel>();
      old_details.push_back(
          winrt::make_weak(detail_rows.Children().GetAt(0).as<TextBlock>()));
      {
        std::lock_guard lock(receiver.mutex);
        receiver.accepted.clear();
        receiver.attempted.clear();
      }
      PROCESS_MEMORY_COUNTERS_EX memory{};
      winrt::check_bool(GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)));
      JsonObject sample;
      sample.Insert(L"tick", JsonValue::CreateNumberValue(tick));
      sample.Insert(L"privateBytes",
                    JsonValue::CreateNumberValue(double(memory.PrivateUsage)));
      samples.Append(sample);
      if (tick == 200) baseline = memory.PrivateUsage;
      if (tick < 1200) return;
      auto state = service->Snapshot();
      if (!state.session || !state.session->session ||
          !state.submitted_frames || !state.metrics_history ||
          state.metrics_history->snapshots.size() < 500)
        throw std::runtime_error(
            "Connected streaming metrics were not exercised");
      if (memory.PrivateUsage > baseline + 24 * 1024 * 1024)
        throw std::runtime_error(
            "Connected page switching retains memory after warmup");
      Finish({});
    } catch (std::exception const& error) {
      Finish(error.what());
    } catch (winrt::hresult_error const& error) {
      Finish(winrt::to_string(error.message()));
    }
  }
  void Finish(std::string const& error) {
    timer.Stop();
    test_exit_code = error.empty() ? 0 : 1;
    JsonObject output;
    output.Insert(L"status", JsonValue::CreateStringValue(
                                 test_exit_code ? L"failed" : L"passed"));
    output.Insert(L"error",
                  JsonValue::CreateStringValue(winrt::to_hstring(error)));
    output.Insert(L"samples", samples);
    output.Insert(L"survivingDetailRows",
                  JsonValue::CreateNumberValue(double(std::count_if(
                      old_details.begin(), old_details.end(),
                      [](auto const& row) { return bool(row.get()); }))));
    if (service) {
      auto state = service->Snapshot();
      output.Insert(L"submittedFrames", JsonValue::CreateNumberValue(
                                            double(state.submitted_frames)));
      output.Insert(L"capturedFrames", JsonValue::CreateNumberValue(
                                           double(state.captured_frames)));
      output.Insert(
          L"historySamples",
          JsonValue::CreateNumberValue(double(
              state.metrics_history ? state.metrics_history->snapshots.size()
                                    : 0)));
    }
    StorageIO::AtomicText(report, winrt::to_string(output.Stringify()));
    if (service) service->Disconnect().get();
    if (window) window.Close();
    Exit();
  }
};
}  // namespace
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int count{};
  auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) return 2;
  if (count == 2) report = arguments[1];
  LocalFree(arguments);
  if (count != 2) return 2;
  if (!report.is_absolute()) return 2;
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  Application::Start([](auto const&) { winrt::make<MemoryApplication>(); });
  return test_exit_code;
}
