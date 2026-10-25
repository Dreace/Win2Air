#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include "../../src/App/OpenSourceNotices.h"
#include "../../src/Settings/Appearance.h"
#include "../../src/Settings/StorageIO.h"
#include "../../src/Shell/TrayService.h"
#include "../../src/UI/MetricFormat.h"
#include "../../src/UI/ViewModels/ReceiverViewModel.h"
#include "../contract/test_support.h"
using namespace Win2Air;
namespace {
winrt::Windows::Data::Xml::Dom::XmlDocument Xml(
    std::filesystem::path const& path) {
  auto bytes = StorageIO::Read(path);
  winrt::Windows::Data::Xml::Dom::XmlDocument value;
  value.LoadXml(winrt::to_hstring(std::string(bytes.begin(), bytes.end())));
  return value;
}
}  // namespace
void TestSettingsAccessibility(wchar_t const* directory) {
  std::set<std::wstring_view> projects;
  for (auto const& notice : kOpenSourceNotices) {
    Require(!notice.name.empty() && !notice.license.empty() &&
                notice.project.starts_with(L"https://") &&
                notice.terms.starts_with(L"https://") &&
                projects.insert(notice.name).second,
            "Open source entries identify a unique project, license and HTTPS "
            "links");
  }
  Require(IsValidReceiverPort(1) && IsValidReceiverPort(7000) &&
              IsValidReceiverPort(65535) && !IsValidReceiverPort(0) &&
              !IsValidReceiverPort(65536) && !IsValidReceiverPort(12.5) &&
              !IsValidReceiverPort(std::numeric_limits<double>::quiet_NaN()),
          "Manual receiver ports accept whole values and reject empty or "
          "fractional input");
  auto root = std::filesystem::path(directory);
  bool custom_budget_found{};
  bool custom_value_found{};
  bool standby_disconnect_found{};
  for (auto const& node : Xml(root / L"src/UI/StreamingSettings.xaml")
                              .GetElementsByTagName(L"*")) {
    auto element = node.as<winrt::Windows::Data::Xml::Dom::XmlElement>();
    if (element.GetAttribute(L"x:Name") == L"StandbyDisconnect") {
      standby_disconnect_found = element.NodeName() == L"ComboBox";
    }
    if (element.GetAttribute(L"x:Name") == L"CustomLatencyValue") {
      custom_value_found = true;
      Require(element.NodeName() == L"NumberBox" &&
                  element.GetAttribute(L"Minimum") ==
                      std::to_wstring(SENDER_MIN_PRESENTATION_MS) &&
                  element.GetAttribute(L"Maximum") ==
                      std::to_wstring(SENDER_MAX_PRESENTATION_MS),
              "The custom delay accepts a typed value in the presentation "
              "range");
    }
    if (element.GetAttribute(L"x:Name") != L"CustomLatency") continue;
    custom_budget_found = true;
    Require(element.NodeName() == L"Slider" &&
                element.GetAttribute(L"StepFrequency") == L"1" &&
                element.GetAttribute(L"Minimum") ==
                    std::to_wstring(SENDER_MIN_PRESENTATION_MS) &&
                element.GetAttribute(L"Maximum") ==
                    std::to_wstring(SENDER_MAX_PRESENTATION_MS),
            "The native budget input exposes the SDK presentation range");
  }
  Require(custom_budget_found && custom_value_found,
          "The custom sender budget input is available");
  Require(standby_disconnect_found,
          "Streaming settings expose the idle disconnect selector");
  for (auto const& file :
       {L"src/UI/StreamingSettings.xaml", L"src/UI/ReceiverCard.xaml"}) {
    for (auto const& node : Xml(root / file).GetElementsByTagName(L"*")) {
      auto element = node.as<winrt::Windows::Data::Xml::Dom::XmlElement>();
      auto name = element.GetAttribute(L"x:Name");
      Require(name != L"DeviceOffset" && name != L"ReceiverOffset" &&
                  name != L"ApplyOffset" && name != L"ResetOffset" &&
                  name != L"OffsetDirection",
              "Latency controls expose the budget without a separate device "
              "adjustment");
    }
  }
  Require(FormatCount(5432112) == L"5,432,112" && FormatCount(0) == L"0" &&
              FormatCount(5432112.4) == L"5,432,112",
          "Counts group as whole numbers");
  Require(FormatQuantity(520320, L"帧") == L"520,320 帧" &&
              FormatQuantity(0, L"包") == L"0 包",
          "Measured quantities keep a grouped number and its unit");
  Rejects([&] { FormatQuantity(1, L""); },
          "A displayed quantity requires a unit");
  auto bitrate_axis = BitrateAxis(1.2e6);
  Require(
      FormatBytes(1.2e9) == L"1.2 GB" &&
          FormatBitrate(1410000) == L"1.41 Mbps" &&
          bitrate_axis.maximum == 2e6 &&
          FormatAxisTick(bitrate_axis.maximum, bitrate_axis) == L"2.0 Mbps" &&
          FormatAxisTick(500000, bitrate_axis) == L"500 kbps",
      "Bytes and bitrate use the display unit from the concept chart");
  Require(FormatAge(0) == L"0s" && FormatAge(60) == L"60s" &&
              FormatAge(600) == L"10 min" && ChartTicks(60).size() == 7 &&
              ChartTicks(60).front() == 60 && ChartTicks(60).back() == 0 &&
              FormatWindowAge(600, 600) == L"10 min" &&
              FormatWindowAge(50, 60) == L"50s",
          "Chart time runs from the window length to now");
  std::vector<std::string> chinese{"zh-Hans-CN"}, french{"fr-FR"},
      english{"en-GB"};
  Require(ResolveLanguage("system", chinese) == "zh-CN" &&
              ResolveLanguage("system", french) == "en-US" &&
              ResolveLanguage("system", english) == "en-US" &&
              ResolveLanguage("en-US", chinese) == "en-US",
          "Language selection honors explicit preferences, supported system "
          "tags and English fallback");
  Rejects([&] { ResolveLanguage("unsupported", chinese); },
          "Unknown saved language is rejected");
  Require(ResolveTheme("system") == ThemePreference::system &&
              ResolveTheme("light") == ThemePreference::light &&
              ResolveTheme("dark") == ThemePreference::dark,
          "Appearance preserves follow-system, light and dark meanings");
  Rejects([&] { ResolveTheme("unsupported"); },
          "Unsupported themes are rejected");
  std::map<std::wstring, std::wstring> controls;
  for (auto const& node :
       Xml(root / L"src/UI/SettingsPage.xaml").GetElementsByTagName(L"*")) {
    auto element = node.as<winrt::Windows::Data::Xml::Dom::XmlElement>();
    auto name = element.GetAttribute(L"x:Name");
    if (!name.empty())
      controls.emplace(name.c_str(), element.TagName().c_str());
  }
  for (auto [name, type] : {std::pair{L"SettingsTheme", L"ComboBox"},
                            {L"SettingsLanguage", L"ComboBox"},
                            {L"SettingsSource", L"ComboBox"},
                            {L"SettingsLocal", L"ToggleSwitch"},
                            {L"StartupEnabled", L"ToggleSwitch"},
                            {L"StartupMinimized", L"ToggleSwitch"},
                            {L"RestorePrevious", L"ToggleSwitch"},
                            {L"CloseToTray", L"ToggleSwitch"}})
    Require(
        controls.contains(name) && controls.at(name) == type,
        "Settings actions use standard keyboard-accessible native controls");
  Require(!controls.contains(L"SettingsPageTitle") &&
              controls.contains(L"SettingsCategory") &&
              controls.at(L"SettingsCategory") == L"ListView" &&
              controls.contains(L"GeneralCategory") &&
              controls.at(L"GeneralCategory") == L"ListViewItem",
          "Settings categories stay beside the selected section");
  bool nested_settings{};
  for (auto const& node :
       Xml(root / L"src/UI/MainWindow.xaml").GetElementsByTagName(L"*")) {
    auto element = node.as<winrt::Windows::Data::Xml::Dom::XmlElement>();
    if (element.GetAttribute(L"x:Name") != L"SettingsNavigation") continue;
    for (auto const& child : element.GetElementsByTagName(L"*")) {
      if (child.as<winrt::Windows::Data::Xml::Dom::XmlElement>().TagName() ==
          L"NavigationViewItem")
        nested_settings = true;
    }
  }
  Require(!nested_settings,
          "The main navigation keeps playback, statistics and settings on one "
          "level");
  Require(Xml(root / L"src/UI/MainWindow.xaml")
                      .DocumentElement()
                      .GetAttribute(L"IsPaneToggleButtonVisible") == L"True" &&
              Xml(root / L"src/UI/MainWindow.xaml")
                      .DocumentElement()
                      .GetAttribute(L"CompactPaneLength") == L"48",
          "The main navigation can collapse to icons");
  bool playback_title{};
  for (auto const& node :
       Xml(root / L"src/UI/PlaybackPage.xaml").GetElementsByTagName(L"*")) {
    if (node.as<winrt::Windows::Data::Xml::Dom::XmlElement>().GetAttribute(
            L"x:Name") == L"PlaybackTitle")
      playback_title = true;
  }
  Require(!playback_title, "Playback omits the page title");
  bool statistics_title_collapsed{};
  for (auto const& node :
       Xml(root / L"src/UI/StatisticsPage.xaml").GetElementsByTagName(L"*")) {
    auto element = node.as<winrt::Windows::Data::Xml::Dom::XmlElement>();
    if (element.GetAttribute(L"x:Name") == L"StatisticsTitle")
      statistics_title_collapsed =
          element.GetAttribute(L"Visibility") == L"Collapsed";
  }
  Require(statistics_title_collapsed,
          "Statistics shows a device name only while a receiver is connected");
  std::set<std::wstring> resource_keys;
  std::map<std::wstring, std::wstring> zh_copy, en_copy;
  for (auto language : {L"en-US", L"zh-CN"}) {
    std::set<std::wstring> keys;
    auto& copy = std::wstring_view(language) == L"zh-CN" ? zh_copy : en_copy;
    for (auto const& node :
         Xml(root / L"resources/Strings" / language / L"Resources.resw")
             .GetElementsByTagName(L"data")) {
      auto element = node.as<winrt::Windows::Data::Xml::Dom::XmlElement>();
      auto name = std::wstring(element.GetAttribute(L"name").c_str());
      auto text = std::wstring(element.InnerText().c_str());
      Require(keys.insert(name).second && !text.empty(),
              "Localized resource names are unique and values are present");
      copy.emplace(name, text);
    }
    if (resource_keys.empty())
      resource_keys = keys;
    else
      Require(
          resource_keys == keys,
          "Chinese and English expose the same localized resource contract");
    for (auto const& entry : copy)
      Require(entry.second.find(L"·") == std::wstring::npos,
              "Localized copy separates fields with a bar");
  }
  for (auto key : {L"SettingsTheme",
                   L"AboutMicrosoft",
                   L"AboutOpenSource",
                   L"AboutProjectLink",
                   L"AboutProjectAddress",
                   L"AboutLicenseLink",
                   L"SettingsLanguage",
                   L"SettingsSource",
                   L"ApplyCustomLatency",
                   L"PreferenceSaved",
                   L"ReceiverForget",
                   L"SwitchOn",
                   L"SwitchOff",
                   L"LatencyOptions",
                   L"MetricDetails",
                   L"AddDevice",
                   L"DevicesNavigation",
                   L"CurrentReceiverCaption",
                   L"StatusPlaying",
                   L"StatusPaused",
                   L"StatusStandby",
                   L"EventPlay",
                   L"EventPause",
                   L"EventStandby",
                   L"EventLatency",
                   L"EventProblemQueue",
                   L"EventProblemResend",
                   L"EventProblemDiscontinuity",
                   L"EventProblemInterrupted",
                   L"EventProblemClock",
                   L"EventProblemRetarget",
                   L"EventConnected",
                   L"EventControlPause",
                   L"EventControlPlay",
                   L"EventControlNext",
                   L"EventControlPrevious",
                   L"EventContent",
                   L"EventStandbyEnter",
                   L"EventStandbyLeave",
                   L"EventStandbyDisconnect",
                   L"ChartNow",
                   L"DialogClose",
                   L"NetworkPendingLabel"})
    Require(resource_keys.contains(key),
            "Settings and pairing actions have localized automation labels and "
            "status copy");
  for (auto key : {L"LatencyLow", L"LatencyUltra", L"LatencyAdaptive",
                   L"LatencyBalanced", L"LatencyStable", L"LatencyCustom"}) {
    auto const& zh = zh_copy.at(key);
    auto const& en = en_copy.at(key);
    auto open = zh.find(L"（"), close = zh.find(L"）");
    Require(open != std::wstring::npos && close != std::wstring::npos &&
                open > 0 && close > open + 1 &&
                en.find(L" (") != std::wstring::npos &&
                en.find(L" ms)") != std::wstring::npos,
            "Latency choices name the mode and the delay");
  }
  Require(zh_copy.at(L"LatencyAdaptive").find(L"{0}") != std::wstring::npos &&
              en_copy.at(L"LatencyAdaptive").find(L"{0}") != std::wstring::npos,
          "Adaptive latency names the recent delay");
  Require(
      zh_copy.at(L"LatencyFieldInvalid") == L"请输入 20 到 5000 之间的整数。" &&
          en_copy.at(L"LatencyFieldInvalid") ==
              L"Enter a whole number from 20 to 5000.",
      "The custom delay field asks for an integer in the presentation range");
  for (auto key :
       {L"PairingDialogTitle", L"PairingDialogHint", L"PairingDialogFailed",
        L"AddReceiverDialogFailed", L"ReceiverHostRequired"})
    Require(zh_copy.contains(key) && !zh_copy.at(key).empty() &&
                en_copy.contains(key) && !en_copy.at(key).empty(),
            "Receiver dialogs provide localized instructions and recovery");
  Require(zh_copy.at(L"UnitFrames") == L"帧" &&
              zh_copy.at(L"UnitPackets") == L"包" &&
              zh_copy.at(L"UnitTimes") == L"次" &&
              zh_copy.at(L"UnitChannels") == L"声道" &&
              zh_copy.at(L"UnitBits") == L"位" &&
              en_copy.at(L"UnitFrames") == L"frames" &&
              en_copy.at(L"UnitPackets") == L"packets" &&
              en_copy.at(L"UnitTimes") == L"times" &&
              en_copy.at(L"UnitChannels") == L"ch" &&
              en_copy.at(L"UnitBits") == L"bit",
          "Audio frames, packets, occurrences, channels and bit depth use "
          "localized units");
  Require(
      zh_copy.at(L"StatusPaused") == L"已连接 | 暂停" &&
          en_copy.at(L"StatusPaused") == L"Connected | Paused" &&
          zh_copy.at(L"EventPlay") == L"播放" &&
          zh_copy.at(L"EventPause") == L"暂停" &&
          zh_copy.at(L"EventStandby") == L"待机" &&
          en_copy.at(L"EventPlay") == L"Play" &&
          en_copy.at(L"EventPause") == L"Pause" &&
          en_copy.at(L"EventStandby") == L"Standby" &&
          zh_copy.at(L"EventLatency") == L"延迟" &&
          en_copy.at(L"EventLatency") == L"Latency" &&
          zh_copy.at(L"EventProblemQueue") == L"发送排队超过预算。" &&
          en_copy.at(L"EventProblemQueue") == L"Send queue above the budget." &&
          zh_copy.at(L"EventProblemResend") == L"重发升高。" &&
          en_copy.at(L"EventProblemResend") == L"Resends increased." &&
          zh_copy.at(L"EventProblemDiscontinuity") == L"音频不连续。" &&
          en_copy.at(L"EventProblemDiscontinuity") == L"Audio discontinuity." &&
          zh_copy.at(L"EventProblemInterrupted") == L"采集中断。" &&
          en_copy.at(L"EventProblemInterrupted") == L"Capture interrupted." &&
          zh_copy.at(L"EventProblemClock") == L"时钟不稳定。" &&
          en_copy.at(L"EventProblemClock") == L"Clock unstable." &&
          zh_copy.at(L"EventProblemRetarget") == L"延迟调整失败。" &&
          en_copy.at(L"EventProblemRetarget") == L"Latency adjustment failed.",
      "Connection events name play, pause, standby, latency and detected "
      "problems");
  for (auto const& key : resource_keys)
    Require(!key.starts_with(L"HealthReason_"),
            "Diagnostic thresholds stay in logs instead of user-facing copy");
  ReceiverRecord addressed;
  addressed.model = "HomePod mini";
  addressed.manual_endpoint =
      ReceiverEndpoint{"living.local", "192.168.1.20", 7000};
  Require(ReceiverDetail(addressed) == "HomePod mini | 192.168.1.20",
          "A device card detail shows the model and address");
  addressed.manual_endpoint->port = 7001;
  Require(ReceiverDetail(addressed) == "HomePod mini | 192.168.1.20:7001",
          "A non-default port stays visible with the address");
  addressed.model.clear();
  addressed.manual_endpoint->resolved_address.clear();
  addressed.manual_endpoint->port = 7000;
  Require(ReceiverDetail(addressed) == "living.local",
          "A device without a resolved address shows the host");
  auto previous_language = resource_language;
  resource_language = "zh-CN";
  ReceiverRecord discovered;
  discovered.model = "AudioAccessory5,1";
  discovered.manual_endpoint =
      ReceiverEndpoint{"living.local", "192.168.1.117", 7000};
  Require(
      ReceiverDetail(discovered) == "HomePod mini（第一代） | 192.168.1.117",
      "AudioAccessory5,1 is shown as HomePod mini in Chinese");
  discovered.model = "AudioAccessory6,1";
  Require(ReceiverDetail(discovered) == "HomePod（第二代） | 192.168.1.117",
          "AudioAccessory6,1 uses the Chinese HomePod generation name");
  resource_language = "en-US";
  Require(
      ReceiverDetail(discovered) == "HomePod (2nd generation) | 192.168.1.117",
      "AudioAccessory6,1 uses the English HomePod generation name");
  discovered.model = "AudioAccessory5,1";
  Require(ReceiverDetail(discovered) ==
              "HomePod mini (1st generation) | 192.168.1.117",
          "AudioAccessory5,1 is shown as HomePod mini in English");
  discovered.model = "Mac14,10";
  Require(ReceiverDetail(discovered) ==
              "MacBook Pro (16-inch, 2023) | 192.168.1.117",
          "MacBook receiver details omit chip and variant information");
  resource_language = "zh-CN";
  Require(ReceiverDetail(discovered) ==
              "MacBook Pro（16 英寸，2023 年） | 192.168.1.117",
          "Chinese MacBook details keep the size and year beside the address");
  resource_language = previous_language;
  PlaybackSnapshot snapshot;
  ReceiverRecord first;
  first.receiver_id = "AA:BB:CC:DD:EE:FF";
  first.name = "Speaker";
  first.model = "Virtual";
  auto hidden = first;
  hidden.receiver_id = "11:22:33:44:55:66";
  hidden.hidden = true;
  snapshot.receivers = {first, hidden};
  auto session = std::make_shared<SessionSnapshot>();
  session->intent.selected_receiver_id = first.receiver_id;
  session->intent.desired_connected = true;
  ReceiverSession active;
  active.receiver_id = first.receiver_id;
  active.state = SessionState::ready;
  session->session = active;
  snapshot.session = session;
  snapshot.volume_pending = true;
  Require(ReceiverViews(snapshot).front().adjustable,
          "Waiting for remote volume keeps receiver controls adjustable");
  Require(ReceiverViews(snapshot, false).size() == 1 &&
              ReceiverViews(snapshot, true).size() == 2,
          "Management can reveal hidden receivers without changing saved "
          "visibility");
  auto menu =
      TrayMenu(snapshot, {L"Open", L"Settings", L"Exit", L"Disconnect"});
  unsigned checked{};
  for (auto const& item : menu)
    if (item.command.action == TrayAction::connect && item.checked) ++checked;
  Require(checked == 1,
          "The tray maintains a single selected saved destination");
  active.state = SessionState::error;
  active.failure_kind = "authentication";
  CommandResult failure;
  failure.message = "setup refused";
  active.error = failure;
  session->session = active;
  auto failed = ReceiverViews(snapshot);
  Require(failed.front().receiver.receiver_id == first.receiver_id &&
              failed.front().recovery_reason == "setup refused",
          "A failed session stays on that device");
  Require(
      DeviceFaultText(failed.front(), "pair again", "retry") == "pair again",
      "The device card requests authentication without protocol diagnostics");
  failed.front().retry_seconds = 4;
  Require(
      DeviceFaultText(failed.front(), "pair again", "retry") ==
          "pair again\nretry 4 s",
      "Retry timing remains visible while raw failures stay out of the card");
}
