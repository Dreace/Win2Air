#include "StreamingSettings.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <cmath>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include "../Settings/StorageIO.h"
#include "../Streaming/LatencyProfile.h"
#include "../Streaming/PresentationConfig.h"
namespace Win2Air {
using namespace winrt::Microsoft::UI::Xaml::Controls;
namespace {
struct LatencyChoice {
  wchar_t const* key;
  char const* mode;
};
constexpr LatencyChoice kLatencyChoices[] = {
    {L"LatencyAdaptive", "adaptive"}, {L"LatencyUltra", "ultra"},
    {L"LatencyLow", "low"},           {L"LatencyBalanced", "balanced"},
    {L"LatencyStable", "stable"},     {L"LatencyCustom", "custom"},
};
constexpr int kCustomLatencyIndex = int(std::size(kLatencyChoices)) - 1;
int LatencyIndex(char const* mode) {
  for (int index = 0; index < int(std::size(kLatencyChoices)); ++index)
    if (std::string_view(kLatencyChoices[index].mode) == mode) return index;
  throw std::logic_error("Latency choice is missing");
}
std::wstring FormatLatency(winrt::hstring const& pattern, int milliseconds) {
  auto text = std::wstring(pattern);
  auto slot = text.find(L"{0}");
  if (slot == std::wstring::npos)
    throw std::runtime_error("Adaptive latency text has no recent value");
  text.replace(slot, 3, std::to_wstring(milliseconds));
  return text;
}
int Integer(double value) {
  if (!std::isfinite(value) || std::floor(value) != value || value < -1000 ||
      value > 5000)
    throw std::runtime_error(
        "Streaming value requires a whole number within its range");
  return static_cast<int>(value);
}
}  // namespace
StreamingSettings::StreamingSettings(StreamCoordinator& service,
                                     std::filesystem::path const& path,
                                     std::shared_ptr<StreamingDraft> draft)
    : model_(service, std::move(draft)) {
  auto bytes = StorageIO::Read(path, 65536);
  root_ = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(
              winrt::to_hstring(std::string(bytes.begin(), bytes.end())))
              .as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
  resources_.ApplyLanguage(root_);
  Find<TextBlock>(L"StreamingTitle")
      .Text(resources_.GetString(L"StreamingTitle"));
  Find<Button>(L"ApplyCustomLatency")
      .Content(winrt::box_value(resources_.GetString(L"ApplyCustomLatency")));
  for (auto name : {L"CustomLatency", L"LatencyMode", L"SilenceTimeout",
                    L"AutoReconnect", L"StandbyDisconnect"}) {
    Find<TextBlock>((std::wstring(name) + L"Label").c_str())
        .Text(resources_.GetString(name));
    winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
        root_.FindName(name).as<winrt::Microsoft::UI::Xaml::DependencyObject>(),
        resources_.GetString(name));
  }
  auto modes = Find<ComboBox>(L"LatencyMode");
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      modes, resources_.GetString(L"LatencyMode"));
  for (auto const& choice : kLatencyChoices)
    modes.Items().Append(winrt::box_value(resources_.GetString(choice.key)));
  modes.SelectionChanged([this](auto const&, auto const&) {
    auto index = Find<ComboBox>(L"LatencyMode").SelectedIndex();
    if (!rendering_ && index >= 0 && index < int(std::size(kLatencyChoices)))
      model_.Draft().mode.Edit(kLatencyChoices[index].mode);
    if (!rendering_) {
      Find<TextBlock>(L"LatencyFieldError").Text(L"");
      input_error_.clear();
    }
    bool custom = index == kCustomLatencyIndex;
    Find<Slider>(L"CustomLatency").IsEnabled(custom);
    Find<NumberBox>(L"CustomLatencyValue").IsEnabled(custom);
    Find<winrt::Microsoft::UI::Xaml::FrameworkElement>(L"CustomLatencyRow")
        .Visibility(custom ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                           : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    Find<Button>(L"ApplyCustomLatency")
        .Visibility(custom ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                           : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    if (!rendering_ && index >= 0 && index < int(std::size(kLatencyChoices))) {
      ApplyLatency();
      Refresh();
    }
  });
  Find<Slider>(L"CustomLatency").ValueChanged([this](auto const&, auto const&) {
    auto value = Integer(Find<Slider>(L"CustomLatency").Value());
    if (!rendering_) model_.Draft().custom.Edit(value);
    auto box = Find<NumberBox>(L"CustomLatencyValue");
    if (box.Value() != value) box.Value(value);
  });
  Find<NumberBox>(L"CustomLatencyValue")
      .ValueChanged([this](NumberBox const& box, auto const&) {
        if (rendering_) return;
        auto field = Find<TextBlock>(L"LatencyFieldError");
        try {
          auto value = Integer(box.Value());
          if (value < SENDER_MIN_PRESENTATION_MS ||
              value > SENDER_MAX_PRESENTATION_MS)
            throw std::runtime_error(
                "Custom delay is outside the presentation range");
          input_error_.clear();
          field.Text(L"");
          field.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
          auto slider = Find<Slider>(L"CustomLatency");
          if (slider.Value() != value)
            slider.Value(value);
          else
            model_.Draft().custom.Edit(value);
        } catch (std::exception const&) {
          input_error_ = resources_.GetString(L"LatencyFieldInvalid").c_str();
          field.Text(input_error_);
          field.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
        }
      });
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      Find<NumberBox>(L"CustomLatencyValue"),
      resources_.GetString(L"CustomLatency"));
  auto timeout = Find<ComboBox>(L"SilenceTimeout");
  for (auto key :
       {L"SilenceTen", L"SilenceThirty", L"SilenceSixty", L"SilenceNever"})
    timeout.Items().Append(winrt::box_value(resources_.GetString(key)));
  Find<Button>(L"ApplyCustomLatency").Click([this](auto const&, auto const&) {
    ApplyLatency();
    Refresh();
  });
  timeout.SelectionChanged([this](auto const&, auto const&) {
    if (rendering_) return;
    const int seconds[] = {10, 30, 60, 0};
    auto index = Find<ComboBox>(L"SilenceTimeout").SelectedIndex();
    if (index >= 0 && index < 4) model_.Silence(seconds[index]);
  });
  auto reconnect = Find<ToggleSwitch>(L"AutoReconnect");
  auto disconnect = Find<ComboBox>(L"StandbyDisconnect");
  for (auto key : {L"SilenceNever", L"StandbyOneMinute", L"StandbyFiveMinutes",
                   L"StandbyFifteenMinutes", L"StandbyThirtyMinutes"})
    disconnect.Items().Append(winrt::box_value(resources_.GetString(key)));
  disconnect.SelectionChanged([this](auto const&, auto const&) {
    if (rendering_) return;
    constexpr int seconds[] = {0, 60, 300, 900, 1800};
    auto index = Find<ComboBox>(L"StandbyDisconnect").SelectedIndex();
    if (index >= 0 && index < int(std::size(seconds)))
      model_.StandbyDisconnect(seconds[index]);
  });
  resources_.LocalizeSwitch(reconnect);
  reconnect.Toggled([this](auto const&, auto const&) {
    if (!rendering_)
      model_.AutoReconnect(Find<ToggleSwitch>(L"AutoReconnect").IsOn());
  });
  Refresh();
}
void StreamingSettings::ApplyLatency() {
  try {
    auto index = Find<ComboBox>(L"LatencyMode").SelectedIndex();
    if (index < 0 || index >= int(std::size(kLatencyChoices)))
      throw std::runtime_error("Select a valid latency mode");
    int custom_ms = model_.Snapshot().playback.preferences.custom_ms;
    if (index == kCustomLatencyIndex) {
      custom_ms = Integer(Find<NumberBox>(L"CustomLatencyValue").Value());
      if (custom_ms < SENDER_MIN_PRESENTATION_MS ||
          custom_ms > SENDER_MAX_PRESENTATION_MS)
        throw std::runtime_error(
            "Custom delay is outside the presentation range");
    }
    model_.Latency(kLatencyChoices[index].mode, custom_ms);
    input_error_.clear();
    Find<TextBlock>(L"LatencyFieldError").Text(L"");
  } catch (std::exception const&) {
    input_error_ = resources_.GetString(L"LatencyFieldInvalid").c_str();
    Find<TextBlock>(L"LatencyFieldError").Text(input_error_);
  }
}
void StreamingSettings::Refresh() {
  model_.Refresh();
  auto const& view = model_.Snapshot();
  auto const& settings = view.playback.preferences;
  rendering_ = true;
  std::optional<int> live;
  if (settings.latency_mode == "adaptive" && view.playback.timing.setup_applied)
    live = view.playback.timing.base_ms;
  auto recent = RecentAdaptiveBudget(settings, view.playback.receivers, live);
  auto modes = Find<ComboBox>(L"LatencyMode");
  auto adaptive = LatencyIndex("adaptive");
  auto label = recent ? winrt::hstring(FormatLatency(
                            resources_.GetString(L"LatencyAdaptive"), *recent))
                      : resources_.GetString(L"LatencyAdaptivePending");
  if (winrt::unbox_value<winrt::hstring>(modes.Items().GetAt(adaptive)) !=
      label)
    modes.Items().SetAt(adaptive, winrt::box_value(label));
  Find<ToggleSwitch>(L"AutoReconnect").IsOn(settings.auto_reconnect);
  constexpr int disconnect_seconds[] = {0, 60, 300, 900, 1800};
  for (int index = 0; index < int(std::size(disconnect_seconds)); ++index)
    if (disconnect_seconds[index] == settings.standby_disconnect_seconds)
      Find<ComboBox>(L"StandbyDisconnect").SelectedIndex(index);
  Find<ComboBox>(L"StandbyDisconnect")
      .IsEnabled(!view.pending && settings.silence_timeout != 0);
  {
    auto const& draft = model_.Draft();
    auto const& mode = draft.mode.Value();
    std::string selected = mode;
    int mode_index = kCustomLatencyIndex;
    for (int index = 0; index < int(std::size(kLatencyChoices)); ++index)
      if (selected == kLatencyChoices[index].mode) mode_index = index;
    modes.SelectedIndex(mode_index);
    Find<Slider>(L"CustomLatency").Value(draft.custom.Value());
    auto box = Find<NumberBox>(L"CustomLatencyValue");
    if (box.Value() != draft.custom.Value()) box.Value(draft.custom.Value());
    Find<ComboBox>(L"SilenceTimeout")
        .SelectedIndex(settings.silence_timeout == 10   ? 0
                       : settings.silence_timeout == 30 ? 1
                       : settings.silence_timeout == 60 ? 2
                                                        : 3);
  }
  for (auto name : {L"ApplyCustomLatency", L"LatencyMode", L"SilenceTimeout"})
    root_.FindName(name).as<Control>().IsEnabled(!view.pending);
  bool connected = view.playback.session && view.playback.session->session;
  for (auto name : {L"LatencyFieldError"}) {
    auto field = Find<TextBlock>(name);
    field.Visibility(field.Text().empty()
                         ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                         : winrt::Microsoft::UI::Xaml::Visibility::Visible);
  }
  std::string command_error = view.error;
  if (connected && view.playback.session->session->error &&
      view.playback.session->session->error->message == command_error)
    command_error.clear();
  if (!view.playback.adaptive_error.empty() &&
      command_error == view.playback.adaptive_error)
    command_error.clear();
  ShowBlockingFailure(root_, command_error,
                      resources_.GetString(L"DialogClose"), blocking_);
  rendering_ = false;
}
}  // namespace Win2Air
