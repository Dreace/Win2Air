#include "SystemAudioValidation.h"

#include <mmsystem.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winternl.h>

#include <cmath>

#include "../Settings/StorageIO.h"
#include "../Streaming/FailureDetail.h"
namespace Win2Air {
using namespace winrt::Windows::Data::Json;
namespace {
void Put(JsonObject const &value, wchar_t const *key, uint64_t number) {
  value.Insert(key, JsonValue::CreateStringValue(
                        winrt::to_hstring(std::to_string(number))));
}
}  // namespace
SystemAudioValidation::SystemAudioValidation(
    StreamCoordinator &service, std::filesystem::path const &config)
    : service_(service) {
  auto bytes = StorageIO::Read(config, 16384);
  auto input = JsonObject::Parse(
      winrt::to_hstring(std::string(bytes.begin(), bytes.end())));
  if (!input.GetNamedBoolean(L"enable_hardware"))
    throw std::runtime_error(
        "Hardware validation requires explicit enable_hardware");
  wave_ = input.GetNamedString(L"wave").c_str();
  output_ = input.GetNamedString(L"report").c_str();
  if (!wave_.is_absolute() || !output_.is_absolute() ||
      !std::filesystem::is_regular_file(wave_))
    throw std::runtime_error(
        "Hardware validation requires absolute audio and report paths");
  auto port = input.GetNamedNumber(L"port");
  if (port < 1 || port > 65535 || std::floor(port) != port)
    throw std::runtime_error("Hardware receiver port invalid");
  report_.Insert(L"run_id", input.GetNamedValue(L"run_id"));
  report_.Insert(L"host", input.GetNamedValue(L"host"));
  report_.Insert(L"port", input.GetNamedValue(L"port"));
  report_.Insert(L"capture",
                 JsonValue::CreateStringValue(L"App WASAPI shared loopback"));
  report_.Insert(L"architecture", JsonValue::CreateStringValue(L"x64"));
  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  auto query = reinterpret_cast<LONG(WINAPI *)(PRTL_OSVERSIONINFOW)>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (!query || query(&version))
    throw std::runtime_error("Read actual Windows version failed");
  Put(report_, L"windows_build", version.dwBuildNumber);
  expected_base_ = static_cast<int>(input.GetNamedNumber(L"base_ms"));
  expected_offset_ = static_cast<int>(input.GetNamedNumber(L"offset_ms"));
  if (input.HasKey(L"continuous_seconds")) {
    auto seconds = input.GetNamedNumber(L"continuous_seconds");
    if (seconds < 30 || seconds > 180 || std::floor(seconds) != seconds)
      throw std::runtime_error(
          "Continuous playback requires 30 through 180 seconds");
    continuous_seconds_ = static_cast<unsigned>(seconds);
  }
  deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  if (continuous_seconds_)
    deadline_ += std::chrono::seconds(continuous_seconds_);
  pending_ =
      service_.AddReceiver(winrt::to_string(input.GetNamedString(L"host")),
                           static_cast<unsigned>(port), expected_offset_);
  Write("in_progress");
}
SystemAudioValidation::~SystemAudioValidation() {
  PlaySoundW(nullptr, nullptr, 0);
}
void SystemAudioValidation::Write(std::string const &status,
                                  std::string const &error) {
  report_.Insert(L"status",
                 JsonValue::CreateStringValue(winrt::to_hstring(status)));
  report_.Insert(L"error",
                 JsonValue::CreateStringValue(winrt::to_hstring(error)));
  StorageIO::AtomicText(output_, winrt::to_string(report_.Stringify()));
}
bool SystemAudioValidation::Tick() {
  try {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline_)
      throw std::runtime_error(
          "App system audio validation exceeded total deadline");
    if (pending_.valid()) {
      if (pending_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready)
        return false;
      auto result = pending_.get();
      if (result.status != CommandStatus::applied)
        throw std::runtime_error(result.message.empty()
                                     ? "System audio validation command failed"
                                     : result.message);
    }
    auto snapshot = service_.Snapshot();
    if (continuous_seconds_ && step_ == 11) {
      if (!snapshot.session || !snapshot.session->session ||
          snapshot.reconnect ||
          snapshot.session->session->generation != continuous_generation_ ||
          snapshot.session->session->state == SessionState::error)
        throw std::runtime_error(
            "Continuous playback lost its original receiver session");
      if (now >= next_) {
        if (snapshot.submitted_frames <= previous_submitted_ ||
            snapshot.nonzero_frames <= previous_nonzero_)
          throw std::runtime_error(
              "Continuous playback stopped submitting fresh audible PCM");
        JsonObject sample;
        Put(sample, L"submitted_frames", snapshot.submitted_frames);
        Put(sample, L"captured_frames", snapshot.captured_frames);
        Put(sample, L"dropped_capture_blocks", snapshot.dropped_capture_blocks);
        Put(sample, L"nonzero_frames", snapshot.nonzero_frames);
        Put(sample, L"discontinuities", snapshot.discontinuities);
        Put(sample, L"sync_packets", snapshot.timing.sync_packets);
        sample.Insert(L"effective_ms", JsonValue::CreateNumberValue(
                                           snapshot.timing.effective_ms));
        continuous_samples_.Append(sample);
        report_.Insert(L"samples", continuous_samples_);
        JsonObject reasons;
        for (auto const &[reason, count] : snapshot.discontinuity_reasons)
          Put(reasons, winrt::to_hstring(reason).c_str(), count);
        report_.Insert(L"discontinuity_reasons", reasons);
        previous_submitted_ = snapshot.submitted_frames;
        previous_nonzero_ = snapshot.nonzero_frames;
        next_ = now + std::chrono::seconds(5);
        Write("in_progress");
      }
      if (now >= deadline_ - std::chrono::seconds(30)) {
        PlaySoundW(nullptr, nullptr, 0);
        pending_ = service_.Disconnect();
        step_ = 12;
      }
      return false;
    }
    if (continuous_seconds_ && step_ == 12) {
      Write("continuous_transport_completed");
      return true;
    }
    if (step_ == 0) {
      if (snapshot.receivers.empty())
        throw std::runtime_error("Manual probe returned no receiver");
      identity_ = snapshot.receivers.back().receiver_id;
      report_.Insert(L"receiver_id", JsonValue::CreateStringValue(
                                         winrt::to_hstring(identity_)));
      report_.Insert(L"model", JsonValue::CreateStringValue(winrt::to_hstring(
                                   snapshot.receivers.back().model)));
      pending_ = service_.Connect(identity_);
      ++step_;
      return false;
    }
    if (step_ == 1) {
      if (!snapshot.session->session || !snapshot.timing.setup_applied)
        return false;
      endpoint_ = snapshot.source.endpoint_id;
      original_ = OpenEndpointVolume(endpoint_)->Read();
      if (snapshot.timing.base_ms != expected_base_ ||
          snapshot.timing.offset_ms != expected_offset_ ||
          snapshot.timing.effective_ms != expected_base_ + expected_offset_ ||
          snapshot.timing.setup_latency_max_samples !=
              static_cast<uint32_t>((expected_base_ + expected_offset_) *
                                    44100 / 1000))
        throw std::runtime_error(
            "Saved initial presentation was not applied to the actual SETUP");
      Put(report_, L"base_ms", snapshot.timing.base_ms);
      report_.Insert(L"offset_ms",
                     JsonValue::CreateNumberValue(snapshot.timing.offset_ms));
      Put(report_, L"effective_ms", snapshot.timing.effective_ms);
      Put(report_, L"buffer_samples", snapshot.timing.buffer_samples);
      Put(report_, L"setup_min_samples",
          snapshot.timing.setup_latency_min_samples);
      Put(report_, L"setup_max_samples",
          snapshot.timing.setup_latency_max_samples);
      Put(report_, L"queue_capacity", snapshot.timing.capacity_bytes);
      if (!PlaySoundW(wave_.c_str(), nullptr,
                      SND_FILENAME | SND_ASYNC | SND_NODEFAULT |
                          (continuous_seconds_ ? SND_LOOP : 0)))
        throw std::runtime_error("Render known audio to Windows output failed");
      if (continuous_seconds_) {
        continuous_generation_ = snapshot.session->session->generation;
        previous_submitted_ = snapshot.submitted_frames;
        previous_nonzero_ = snapshot.nonzero_frames;
        report_.Insert(L"acoustic_observation",
                       JsonValue::CreateStringValue(L"unobserved"));
        deadline_ = now + std::chrono::seconds(continuous_seconds_ + 30);
        next_ = now + std::chrono::seconds(5);
        step_ = 11;
        return false;
      }
      next_ = now + std::chrono::seconds(10);
      ++step_;
      return false;
    }
    if (step_ == 2 && now >= next_) {
      if (!snapshot.submitted_frames || !snapshot.nonzero_frames)
        throw std::runtime_error(
            "App loopback did not submit actual nonzero system audio");
      Put(report_, L"unmuted_nonzero_frames", snapshot.nonzero_frames);
      AudioSourceConfig source;
      source.follow_system_default = false;
      source.endpoint_id = endpoint_;
      pending_ = service_.SetSource(source);
      ++step_;
      return false;
    }
    if (step_ == 3) {
      pending_ = service_.SetLocalPlayback(false);
      ++step_;
      return false;
    }
    if (step_ == 4) {
      if (!OpenEndpointVolume(endpoint_)->Read().muted)
        throw std::runtime_error(
            "Local playback switch did not mute the selected endpoint");
      muted_nonzero_ = snapshot.nonzero_frames;
      if (!PlaySoundW(wave_.c_str(), nullptr,
                      SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
        throw std::runtime_error("Render muted-loopback input failed");
      next_ = now + std::chrono::seconds(10);
      ++step_;
      return false;
    }
    if (step_ == 5 && now >= next_) {
      if (!snapshot.timing.sync_packets || !snapshot.timing.sync_capture_ns ||
          snapshot.timing.sync_current_pos !=
              static_cast<uint32_t>(snapshot.timing.sync_next_rtp +
                                    snapshot.timing.sync_pending_samples -
                                    snapshot.timing.buffer_samples -
                                    snapshot.timing.offset_ms * 44100 / 1000) ||
          snapshot.timing.sync_minimum_rtp != snapshot.timing.sync_next_rtp)
        throw std::runtime_error(
            "Actual sent RTP sync does not match the initial presentation "
            "budget");
      Put(report_, L"sync_packets", snapshot.timing.sync_packets);
      Put(report_, L"sync_capture_ns", snapshot.timing.sync_capture_ns);
      Put(report_, L"sync_current_pos", snapshot.timing.sync_current_pos);
      Put(report_, L"sync_minimum_rtp", snapshot.timing.sync_minimum_rtp);
      Put(report_, L"sync_next_rtp", snapshot.timing.sync_next_rtp);
      Put(report_, L"sync_pending_samples",
          snapshot.timing.sync_pending_samples);
      Put(report_, L"muted_nonzero_frames",
          snapshot.nonzero_frames - muted_nonzero_);
      if (snapshot.nonzero_frames <= muted_nonzero_)
        throw std::runtime_error(
            "Endpoint mute removed the remote source audio");
      Put(report_, L"captured_frames", snapshot.captured_frames);
      Put(report_, L"submitted_frames", snapshot.submitted_frames);
      Put(report_, L"timeline_epoch", snapshot.timing.timeline_epoch);
      Put(report_, L"first_capture_ns", snapshot.timing.first_capture_ns);
      Put(report_, L"next_sample", snapshot.timing.next_sample);
      Put(report_, L"discontinuities", snapshot.discontinuities);
      pending_ = service_.Disconnect();
      ++step_;
      return false;
    }
    if (step_ == 6) {
      auto restored = OpenEndpointVolume(endpoint_)->Read();
      if (restored.muted != original_.muted ||
          std::abs(restored.volume - original_.volume) > 0.0001f)
        throw std::runtime_error(
            "Disconnect did not restore the original local endpoint snapshot");
      pending_ = service_.SetLocalPlayback(true);
      ++step_;
      return false;
    }
    if (step_ == 7) {
      pending_ = service_.Connect(identity_);
      ++step_;
      return false;
    }
    if (step_ == 8) {
      if (!snapshot.timing.setup_applied || !snapshot.session->session)
        return false;
      if (snapshot.timing.base_ms != expected_base_ ||
          snapshot.timing.offset_ms != expected_offset_)
        throw std::runtime_error(
            "Reconnect did not reapply saved initial presentation");
      if (!PlaySoundW(wave_.c_str(), nullptr,
                      SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
        throw std::runtime_error("Render reconnect audio failed");
      next_ = now + std::chrono::seconds(10);
      ++step_;
      return false;
    }
    if (step_ == 9 && now >= next_) {
      pending_ = service_.Disconnect();
      ++step_;
      return false;
    }
    if (step_ == 10) {
      JsonObject reasons;
      for (auto const &entry : snapshot.discontinuity_reasons)
        Put(reasons, winrt::to_hstring(entry.first).c_str(), entry.second);
      report_.Insert(L"discontinuity_reasons", reasons);
      Put(report_, L"discontinuities", snapshot.discontinuities);
      Put(report_, L"captured_frames", snapshot.captured_frames);
      Put(report_, L"submitted_frames", snapshot.submitted_frames);
      Put(report_, L"future_hold_max_ns", snapshot.future_hold_max_ns);
      Write("awaiting_observation");
      return true;
    }
    return false;
  } catch (...) {
    auto detail = DescribeFailure();
    PlaySoundW(nullptr, nullptr, 0);
    Write("failed", detail.message);
    service_.Disconnect();
    throw;
  }
}
}  // namespace Win2Air
