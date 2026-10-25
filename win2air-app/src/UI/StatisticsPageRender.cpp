#include <shobjidl.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <limits>
#include <stdexcept>
#include <string_view>
#include "../Settings/StorageIO.h"
#include "../Streaming/FailureDetail.h"
#include "MetricFormat.h"
#include "StatisticsPage.h"
#include "ViewModels/ReceiverViewModel.h"
namespace Win2Air {
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
namespace Media = winrt::Microsoft::UI::Xaml::Media;
namespace {
wchar_t const* SessionStatusKey(SessionState state) {
  switch (state) {
    case SessionState::connecting:
      return L"StateConnecting";
    case SessionState::pairing:
      return L"StatePairing";
    case SessionState::setting_up:
      return L"StateSettingUp";
    case SessionState::ready:
      return L"StatusStandby";
    case SessionState::streaming:
      return L"StatusPlaying";
    case SessionState::ending:
      return L"StateEnding";
    case SessionState::recovering:
      return L"StateRecovering";
    case SessionState::error:
      return L"StateError";
    default:
      return L"StateDisconnected";
  }
}
}  // namespace
void StatisticsPage::Render() {
  if (rendering_) return;
  rendering_ = true;
  auto const& state = model_.Snapshot();
  auto const& m = state.metrics;
  auto set_text = [this](wchar_t const* name, auto const& value) {
    root_.FindName(name).as<TextBlock>().Text(value);
  };
  auto known = [](MetricValue const& value) {
    return value.value && std::isfinite(*value.value);
  };
  auto show = [this](wchar_t const* name, bool visible) {
    root_.FindName(name).as<FrameworkElement>().Visibility(
        visible ? Visibility::Visible : Visibility::Collapsed);
  };
  auto shown_number = [known](MetricValue const& value) {
    return known(value) ? *value.value : 0.0;
  };
  auto receiver = std::find_if(
      state.receivers.begin(), state.receivers.end(),
      [&](auto const& item) { return item.receiver_id == m.receiver_id; });
  bool active_session = state.session && state.session->session &&
                        state.session->intent.desired_connected;
  bool named = active_session && receiver != state.receivers.end();
  auto title = root_.FindName(L"StatisticsTitle").as<TextBlock>();
  title.Text(named ? winrt::to_hstring(receiver->name) : L"");
  title.Visibility(named ? Visibility::Visible : Visibility::Collapsed);
  set_text(L"StatisticsReceiver",
           named ? winrt::to_hstring(ReceiverDetail(*receiver))
                 : resources_.GetString(L"NoActiveReceiver"));
  auto stage =
      named ? state.session->session->state : SessionState::disconnected;
  auto standby =
      named ? state.session->session->standby_state : StandbyState::active;
  auto connected = named ? ConnectedStatusKey(stage, standby) : nullptr;
  set_text(L"StatisticsStatus",
           resources_.GetString(connected ? connected
                                : named   ? SessionStatusKey(stage)
                                          : L"StateDisconnected"));
  bool live = named && (stage == SessionState::ready ||
                        stage == SessionState::streaming);
  show(L"StatisticsStatus", named);
  show(L"StatisticsStatusDot", live);
  if (named)
    root_.FindName(L"StatisticsStatus")
        .as<TextBlock>()
        .Foreground(live ? root_.FindName(L"StatisticsStatusDot")
                               .as<Shapes::Ellipse>()
                               .Fill()
                         : root_.FindName(L"StatisticsReceiver")
                               .as<TextBlock>()
                               .Foreground());
  auto seconds = active_session && !m.receiver_id.empty()
                     ? m.uptime / 1000000000
                     : uint64_t{0};
  set_text(L"DurationValue", std::format(L"{:02}:{:02}:{:02}", seconds / 3600,
                                         seconds / 60 % 60, seconds % 60));
  set_text(L"LeadValue",
           FormatCount(active_session ? shown_number(m.target_latency) : 0) +
               L" ms");
  set_text(L"BitrateValue", FormatBitrate(shown_number(m.bitrate)));
  set_text(L"NetworkBytesValue", FormatBytes(shown_number(m.sent_bytes)));
  auto packets = std::wstring(resources_.GetString(L"UnitPackets"));
  set_text(L"NetworkPacketsValue",
           FormatQuantity(shown_number(m.sent_packets), packets));
  set_text(L"NetworkPendingValue",
           FormatQuantity(shown_number(m.pending), packets));
  show(L"NetworkCard", true);
  NoteConnections(state);
  auto events = root_.FindName(L"ConnectionEventRows").as<StackPanel>();
  events.Children().Clear();
  for (auto const& event : connection_events_) {
    Grid row;
    row.ColumnSpacing(16);
    ColumnDefinition clock_column;
    clock_column.Width(GridLength{0, GridUnitType::Auto});
    ColumnDefinition text_column;
    text_column.Width(GridLength{1, GridUnitType::Star});
    row.ColumnDefinitions().Append(clock_column);
    row.ColumnDefinitions().Append(text_column);
    TextBlock clock;
    clock.Text(event.clock);
    clock.FontSize(13);
    clock.Foreground(
        root_.FindName(L"StatisticsReceiver").as<TextBlock>().Foreground());
    TextBlock label;
    label.Text(event.text);
    label.TextWrapping(TextWrapping::Wrap);
    label.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(label, 1);
    row.Children().Append(clock);
    row.Children().Append(label);
    events.Children().Append(row);
  }
  auto health_key = state.health.level == "poor"        ? L"HealthPoor"
                    : state.health.level == "unstable"  ? L"HealthUnstable"
                    : state.health.level == "good"      ? L"HealthGood"
                    : state.health.level == "excellent" ? L"HealthExcellent"
                                                        : L"HealthUnknown";
  auto health = root_.FindName(L"StatisticsHealth").as<TextBlock>();
  health.Text(resources_.GetString(health_key));
  bool health_known = std::wstring_view(health_key) != L"HealthUnknown";
  auto health_color = [&] {
    bool dark = root_.ActualTheme() == ElementTheme::Dark;
    auto paint = [](uint8_t r, uint8_t g, uint8_t b) {
      return Media::SolidColorBrush(winrt::Windows::UI::Color{255, r, g, b});
    };
    if (std::wstring_view(health_key) == L"HealthPoor")
      return paint(dark ? 0xFF : 0xB4, dark ? 0x98 : 0x23, dark ? 0x96 : 0x18);
    if (std::wstring_view(health_key) == L"HealthUnstable")
      return paint(dark ? 0xF0 : 0x94, dark ? 0xBC : 0x62, dark ? 0x6B : 0x00);
    if (health_known)
      return paint(dark ? 0x75 : 0x13, dark ? 0xD5 : 0x73, dark ? 0x9A : 0x33);
    return root_.FindName(L"StatisticsReceiver")
        .as<TextBlock>()
        .Foreground()
        .as<Media::SolidColorBrush>();
  }();
  health.Foreground(health_color);
  auto dot = root_.FindName(L"HealthDot").as<Shapes::Ellipse>();
  dot.Fill(health_color);
  dot.Visibility(health_known ? Visibility::Visible : Visibility::Collapsed);
  auto metrics_error = state.metrics_error;
  if (!state.diagnostics_error.empty()) {
    if (!metrics_error.empty()) metrics_error += '\n';
    metrics_error += state.diagnostics_error;
  }
  ShowBlockingFailure(root_, metrics_error,
                      resources_.GetString(L"DialogClose"), metrics_prompt_);
  auto details = root_.FindName(L"MetricDetailsRows").as<StackPanel>();
  details.Children().Clear();
  TextBlock summary;
  summary.Text(std::wstring(resources_.GetString(L"MetricSessionSummary")) +
               L": " + std::to_wstring(m.format.rate) + L" Hz | " +
               std::to_wstring(m.format.channels) + L" " +
               std::wstring(resources_.GetString(L"UnitChannels")) + L" | " +
               std::to_wstring(m.format.bits) + L" " +
               std::wstring(resources_.GetString(L"UnitBits")));
  summary.TextWrapping(TextWrapping::Wrap);
  summary.IsTextSelectionEnabled(true);
  details.Children().Append(summary);
  auto add_count = [&](wchar_t const* key, MetricValue const& value,
                       std::wstring_view unit) {
    TextBlock text;
    text.TextWrapping(TextWrapping::Wrap);
    text.IsTextSelectionEnabled(true);
    text.Text(std::wstring(resources_.GetString(key)) + L": " +
              FormatQuantity(shown_number(value), unit));
    details.Children().Append(text);
  };
  auto times = std::wstring(resources_.GetString(L"UnitTimes"));
  auto frames = std::wstring(resources_.GetString(L"UnitFrames"));
  add_count(L"MetricResendRequests", m.resend_requests, packets);
  add_count(L"MetricResendSuccess", m.resend_success, packets);
  add_count(L"MetricResendExpired", m.resend_expired, packets);
  add_count(L"MetricDiscontinuities", m.capture_discontinuities, times);
  add_count(L"MetricRecoveries", m.reconnect_count, times);
  add_count(L"MetricCaptured", m.captured_frames, frames);
  add_count(L"MetricSubmitted", m.submitted_frames, frames);
  add_count(L"MetricNonzero", m.nonzero_frames, frames);
  show(L"MetricDetails", true);
  auto draw_history = [&](auto const& chart, HistoryMetric metric,
                          unsigned seconds, wchar_t const* prefix) {
    const wchar_t* keys[] = {L"ChartSentBytes", L"ChartSentPackets",
                             L"LeadLabel", L"BitrateLabel", L"ResendHeading"};
    auto label = resources_.GetString(keys[static_cast<int>(metric)]);
    root_.FindName(winrt::hstring(prefix) + L"Current")
        .as<TextBlock>()
        .Text(resources_.GetString(seconds == 60    ? L"ChartIntervalSecond"
                                   : seconds == 600 ? L"ChartIntervalTenSeconds"
                                                    : L"ChartIntervalMinute"));
    std::vector<double> ages, values;
    if (state.metrics_history) {
      for (auto const& bucket :
           AggregateHistory(*state.metrics_history, m.sample_time, seconds,
                            m.receiver_id, metric)) {
        ages.push_back(bucket.age_seconds);
        values.push_back(
            bucket.value.value_or(std::numeric_limits<double>::quiet_NaN()));
      }
    }
    chart.Update(ages, values, seconds, static_cast<int>(metric), label,
                 root_.ActualTheme() == ElementTheme::Dark);
  };
  draw_history(chart_, metric_, window_seconds_, L"History");
  draw_history(audio_chart_, audio_metric_, audio_window_seconds_,
               L"AudioHistory");
  auto const& history = model_.History();
  auto rate = history.snapshots.size() > 1
                  ? ResendRequestsPerSecond(
                        *history.snapshots[history.snapshots.size() - 2],
                        *history.snapshots.back())
                  : std::optional<double>{};
  set_text(L"NetworkRequestsValue", rate ? FormatRate(*rate) : L"—");
  rendering_ = false;
}
void StatisticsPage::NoteConnections(PlaybackSnapshot const& state) {
  SYSTEMTIME time;
  GetLocalTime(&time);
  auto clock =
      std::format(L"{:02}:{:02}:{:02}", time.wHour, time.wMinute, time.wSecond);
  auto add = [&](std::wstring text, std::wstring event_clock = {}) {
    connection_events_.insert(
        connection_events_.begin(),
        ConnectionEvent{event_clock.empty() ? clock : event_clock,
                        std::move(text)});
    if (connection_events_.size() > 12) connection_events_.resize(12);
  };
  auto endpoint_text = [](ReceiverRecord const& receiver) {
    ReceiverEndpoint const* endpoint =
        receiver.manual_endpoint ? &*receiver.manual_endpoint
        : receiver.discovered_endpoints.empty()
            ? nullptr
            : &receiver.discovered_endpoints.front();
    if (!endpoint || !endpoint->port) return std::wstring{};
    auto address = endpoint->resolved_address.empty()
                       ? endpoint->host
                       : endpoint->resolved_address;
    if (address.empty()) return std::wstring{};
    return std::wstring(winrt::to_hstring(address).c_str()) + L":" +
           std::to_wstring(endpoint->port);
  };
  auto subject = [&](ReceiverRecord const* receiver, std::string const& id) {
    std::wstring line =
        receiver && !receiver->name.empty()
            ? std::wstring(winrt::to_hstring(receiver->name).c_str())
            : std::wstring(winrt::to_hstring(id).c_str());
    if (receiver) {
      auto place = endpoint_text(*receiver);
      if (!place.empty()) line += L" | " + place;
    }
    return line;
  };
  for (auto const& receiver : state.receivers) {
    if (receiver.receiver_id.empty() ||
        std::find(seen_receivers_.begin(), seen_receivers_.end(),
                  receiver.receiver_id) != seen_receivers_.end())
      continue;
    seen_receivers_.push_back(receiver.receiver_id);
    if (!seeded_receivers_) continue;
    std::wstring line(resources_.GetString(L"ConnectionFound"));
    line += L" | ";
    line += subject(&receiver, receiver.receiver_id);
    add(std::move(line));
  }
  seeded_receivers_ = true;
  bool has = state.session && state.session->session;
  auto id = has ? state.session->session->receiver_id : std::string{};
  auto stage = has ? state.session->session->state : SessionState::disconnected;
  auto notice_key = [](std::string const& code) -> wchar_t const* {
    if (code == "latency") return L"EventLatency";
    if (code == "resend") return L"EventProblemResend";
    if (code == "queue") return L"EventProblemQueue";
    if (code == "discontinuity") return L"EventProblemDiscontinuity";
    if (code == "interrupted") return L"EventProblemInterrupted";
    if (code == "clock") return L"EventProblemClock";
    if (code == "retarget") return L"EventProblemRetarget";
    if (code == "control-pause") return L"EventControlPause";
    if (code == "control-play") return L"EventControlPlay";
    if (code == "control-next") return L"EventControlNext";
    if (code == "control-previous") return L"EventControlPrevious";
    if (code == "content") return L"EventContent";
    if (code == "standby-enter") return L"EventStandbyEnter";
    if (code == "standby-leave") return L"EventStandbyLeave";
    if (code == "standby-disconnect") return L"EventStandbyDisconnect";
    return nullptr;
  };
  for (auto const& notice : state.notices) {
    if (!notice.id || notice.id <= noted_notice_) continue;
    noted_notice_ = notice.id;
    auto key = notice_key(notice.code);
    if (!key) continue;
    auto receiver = std::find_if(
        state.receivers.begin(), state.receivers.end(), [&](auto const& item) {
          return item.receiver_id == notice.receiver_id;
        });
    auto line =
        subject(receiver == state.receivers.end() ? nullptr : &*receiver,
                notice.receiver_id) +
        L" | " + std::wstring(resources_.GetString(key));
    if (notice.code == "latency")
      line += L" " + std::to_wstring(notice.latency_ms) + L" ms";
    if (!notice.detail.empty())
      line += L" | " + std::wstring(winrt::to_hstring(notice.detail));
    auto stamp = std::chrono::system_clock::to_time_t(notice.occurred_at);
    std::tm local{};
    localtime_s(&local, &stamp);
    add(std::move(line), std::format(L"{:02}:{:02}:{:02}", local.tm_hour,
                                     local.tm_min, local.tm_sec));
  }
  if (!has) {
    if (noted_session_) {
      auto line =
          noted_subject_.empty()
              ? std::wstring(resources_.GetString(L"StateDisconnected"))
              : noted_subject_ + L" | " +
                    std::wstring(resources_.GetString(L"StateDisconnected"));
      add(std::move(line));
      noted_session_ = false;
      noted_session_id_.clear();
      noted_subject_.clear();
      noted_action_.clear();
      noted_state_ = SessionState::disconnected;
    }
    return;
  }
  wchar_t const* action = L"StateDisconnected";
  if (stage == SessionState::ready)
    action = L"EventConnected";
  else if (stage == SessionState::streaming)
    action = L"EventPlay";
  else if (stage == SessionState::connecting)
    action = L"StateConnecting";
  else if (stage == SessionState::pairing)
    action = L"StatePairing";
  else if (stage == SessionState::setting_up)
    action = L"StateSettingUp";
  else if (stage == SessionState::recovering)
    action = L"StateRecovering";
  else if (stage == SessionState::ending)
    action = L"StateEnding";
  else if (stage == SessionState::error)
    action = L"StateError";
  if (!(noted_session_ && id == noted_session_id_ && noted_action_ == action)) {
    auto receiver =
        std::find_if(state.receivers.begin(), state.receivers.end(),
                     [&](auto const& item) { return item.receiver_id == id; });
    noted_subject_ =
        subject(receiver == state.receivers.end() ? nullptr : &*receiver, id);
    if (stage != SessionState::disconnected ||
        state.session->session->standby_state != StandbyState::standby)
      add(noted_subject_ + L" | " + std::wstring(resources_.GetString(action)));
    noted_session_ = true;
    noted_session_id_ = std::move(id);
    noted_action_ = action;
    noted_state_ = stage;
  }
}
}  // namespace Win2Air
