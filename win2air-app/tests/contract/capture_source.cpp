#include "../../src/Audio/WasapiCapture.h"
#include "../../src/Streaming/PresentationConfig.h"
#include "../fixtures/AudioFixture.h"
#include "test_support.h"
using namespace Win2Air;
using namespace Win2Air::Tests;
void TestCaptureSource() {
  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nSamplesPerSec = 44100;
  format.wBitsPerSample = 16;
  format.nChannels = 2;
  format.nBlockAlign = 4;
  format.nAvgBytesPerSec = 176400;
  auto silent = CopyCapturedPacket(
      nullptr, 441,
      AUDCLNT_BUFFERFLAGS_SILENT | AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, 37,
      10000000, format, 1);
  Require(silent.owned_bytes.size() == 1764 &&
              silent.capture_100ns == 10000000 &&
              silent.device_frame_position == 37,
          "Capture preserves frames and 100ns timestamp");
  for (auto value : silent.owned_bytes)
    Require(value == 0, "SILENT produces owned digital silence");
  std::vector<uint8_t> bytes(1764, 7);
  auto copy = CopyCapturedPacket(
      bytes.data(), 441, AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR, 0, 0, format, 1);
  bytes.assign(bytes.size(), 0);
  Require(copy.owned_bytes.front() == 7 &&
              (copy.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR),
          "Capture owns data after driver buffer release");
  Rejects([&] { CopyCapturedPacket(nullptr, 441, 0, 0, 0, format, 1); },
          "Absent nonsilent driver buffer rejected");
  auto virtual_audio = std::make_shared<VirtualAudioState>();
  VirtualDevices devices(virtual_audio);
  AudioSourceConfig source;
  Require(devices.Resolve(source) == "virtual-default",
          "Default capture selects the virtual multimedia output");
  source.follow_system_default = false;
  source.endpoint_id = "virtual-specified";
  Require(devices.Resolve(source) == "virtual-specified",
          "Specified capture selects its exact virtual endpoint");
  source.role = "communications";
  Rejects([&] { devices.Resolve(source); },
          "Capture source rejects a role outside multimedia");
  source.role = "multimedia";
  source.follow_system_default = false;
  source.endpoint_id.clear();
  Rejects([&] { devices.Resolve(source); },
          "Specified source requires an endpoint identity");
  source.endpoint_id = "missing-render-endpoint";
  Rejects([&] { devices.Resolve(source); },
          "Unavailable specified source does not switch to system default");
  devices.Notify({"controlled-render", DEVICE_STATE_NOTPRESENT, false, false});
  devices.Notify({"controlled-render", 0, false, true});
  auto events = devices.TakeEvents();
  bool removal = false, changed = false;
  for (auto const& event : events)
    if (event.endpoint_id == "controlled-render") {
      removal |= event.state == DEVICE_STATE_NOTPRESENT;
      changed |= event.format_changed;
    }
  Require(removal && changed,
          "Source removal and format notifications are owned events on the "
          "serial context");
  source = {};
  virtual_audio->ChangeDefault("virtual-specified");
  Require(devices.Resolve(source) == "virtual-specified",
          "Following mode resolves the changed default endpoint");
  source.follow_system_default = false;
  source.endpoint_id = "virtual-default";
  virtual_audio->Remove("virtual-default");
  Rejects([&] { devices.Resolve(source); },
          "Removed specified source stays unavailable without fallback");

  auto driver = std::make_shared<VirtualAudioState>();
  WasapiCapture capture(
      [driver] { return std::make_unique<VirtualCaptureBackend>(driver); });
  capture.Start("virtual-default", 11);
  auto packet = NonzeroPacket(441, 12345678);
  packet.position = 99;
  driver->Push(std::move(packet));
  std::optional<CapturedBlock> delivered;
  WaitUntil(
      [&] {
        delivered = capture.Pop();
        return delivered.has_value();
      },
      "Virtual capture worker did not deliver its packet");
  Require(delivered->owned_bytes[1] == 16 && delivered->source_epoch == 11 &&
              delivered->capture_100ns == 12345678 &&
              delivered->device_frame_position == 99,
          "Production capture worker preserves source data after driver memory "
          "is poisoned and released");
  VirtualPacket zero;
  zero.frames = 441;
  zero.flags = AUDCLNT_BUFFERFLAGS_SILENT;
  zero.timestamp = 12355678;
  driver->Push(std::move(zero));
  WaitUntil(
      [&] {
        delivered = capture.Pop();
        return delivered.has_value();
      },
      "Virtual silent packet was not delivered");
  for (auto byte : delivered->owned_bytes)
    Require(byte == 0,
            "Driver SILENT flag creates owned zero PCM on the worker");
  {
    std::lock_guard lock(driver->mutex);
    driver->fail_next = true;
  }
  WaitUntil(
      [&] {
        try {
          capture.Pop();
          return false;
        } catch (winrt::hresult_error const& error) {
          return error.code() == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        }
      },
      "Virtual driver removal error did not reach the capture consumer");
  capture.Stop();
  {
    std::lock_guard lock(driver->mutex);
    Require(
        driver->released == 2 && driver->stopped == 1 && !driver->wrong_thread,
        "Driver acquisition, release and stop stay on the capture worker "
        "thread");
    driver->fail_next = false;
    driver->fail_release = true;
  }
  capture.Start("virtual-default", 12);
  driver->Push(NonzeroPacket(441));
  WaitUntil(
      [&] {
        try {
          capture.Pop();
          return false;
        } catch (std::runtime_error const&) {
          return true;
        }
      },
      "Driver release failure did not stop the capture worker");
  capture.Stop();
  {
    std::lock_guard lock(driver->mutex);
    Require(driver->released == 3 && driver->stopped == 2,
            "Release failure still closes the virtual driver");
    driver->fail_release = false;
  }
  capture.Start("virtual-default", 13);
  VirtualPacket invalid;
  invalid.frames = 1;
  invalid.timestamp = 12345678;
  driver->Push(std::move(invalid));
  WaitUntil(
      [&] {
        try {
          capture.Pop();
          return false;
        } catch (std::runtime_error const&) {
          return true;
        }
      },
      "Invalid borrowed packet did not fail capture");
  capture.Stop();
  {
    std::lock_guard lock(driver->mutex);
    Require(driver->released == 4 && driver->stopped == 3,
            "Copy validation failure releases the acquired driver buffer");
    driver->fail_open = true;
  }
  Rejects([&] { capture.Start("virtual-default", 14); },
          "Driver open failure is returned by Start without a leaked worker");

  auto buffered = std::make_shared<VirtualAudioState>();
  WasapiCapture bounded(
      [buffered] { return std::make_unique<VirtualCaptureBackend>(buffered); });
  std::atomic<unsigned> notifications{};
  bounded.OnReady([&] { ++notifications; });
  Rejects([&] { bounded.Start("virtual-default", 15, 4); },
          "Capture budget rejects an unsupported bound before opening");
  bounded.Start("virtual-default", 15, 20);
  Rejects([&] { bounded.OnReady({}); },
          "A running capture keeps its notification lifetime stable");
  for (unsigned index = 0; index < 4; ++index) {
    auto incoming = NonzeroPacket(441, 10000000 + index * 100000);
    incoming.position = index * 441;
    buffered->Push(std::move(incoming));
  }
  WaitUntil([&] { return notifications.load() == 4; },
            "Capture completion notifies without waiting for consumer polling");
  Require(!bounded.Pop() && bounded.DroppedBlocks() == 4,
          "SPSC overflow drops incoming packets and the consumer removes "
          "audio older than its live budget");
  buffered->Push(NonzeroPacket(441, 10400000));
  buffered->Push(NonzeroPacket(441, 10500000));
  WaitUntil([&] { return notifications.load() == 6; },
            "Capture delivers queued audio before changing its live budget");
  bounded.SetBufferBudget(10);
  auto current = bounded.Pop();
  Require(current && !bounded.Pop() && bounded.DroppedBlocks() == 5 &&
              current->capture_100ns == 10500000 &&
              (current->flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY),
          "A live budget reduction keeps the newest frame and records its gap");
  bounded.Stop();

  auto coalesced = std::make_shared<VirtualAudioState>();
  WasapiCapture strict_budget([coalesced] {
    return std::make_unique<VirtualCaptureBackend>(coalesced);
  });
  strict_budget.Start("virtual-default", 16, 10);
  auto large = NonzeroPacket(1323, 20000000);
  large.position = 700;
  coalesced->Push(std::move(large));
  WaitUntil(
      [&] {
        try {
          strict_budget.Pop();
        } catch (std::runtime_error const& error) {
          auto message = std::string(error.what());
          return message.find("1323 frames") != std::string::npos &&
                 message.find("10 ms") != std::string::npos;
        }
        return false;
      },
      "A driver packet above the budget must fail with its actual frame count");
  strict_budget.Stop();
  {
    std::lock_guard lock(coalesced->mutex);
    Require(coalesced->released == 1 && coalesced->stopped == 1,
            "Rejecting an unsupported packet releases the driver and stops "
            "capture without trimming valid audio");
  }
  strict_budget.Start("virtual-default", 17, 5);
  coalesced->Push(NonzeroPacket(221, 20300000));
  WaitUntil(
      [&] {
        delivered = strict_budget.Pop();
        return delivered.has_value();
      },
      "A fractional frame budget must keep the latest complete frames");
  Require(delivered->frame_count == 221 && delivered->owned_bytes.size() == 884,
          "A fractional millisecond boundary retains complete source frames");
  strict_budget.Stop();
  coalesced->period.default_frames = 882;
  Rejects([&] { strict_budget.Start("virtual-default", 18, 10); },
          "An observed engine period above the budget fails during startup");
  {
    std::lock_guard lock(coalesced->mutex);
    Require(coalesced->released == 2 && coalesced->stopped == 3,
            "Unsupported period startup closes its opened driver");
  }

  // Windows can deliver multiple engine periods after a scheduling delay.
  // Retain every frame still inside the presentation deadline and consume it
  // immediately, without waiting for the ring to fill.
  auto burst = std::make_shared<VirtualAudioState>();
  WasapiCapture burst_capture(
      [burst] { return std::make_unique<VirtualCaptureBackend>(burst); });
  std::atomic<unsigned> burst_notifications{};
  burst_capture.OnReady([&] { ++burst_notifications; });
  PresentationConfig low_latency;
  low_latency.mode = "ultra";
  low_latency.base_ms = low_latency.effective_ms = 120;
  burst_capture.Start("virtual-default", 19, CaptureBufferMs(low_latency));
  for (unsigned index = 0; index < 8; ++index) {
    auto burst_packet = NonzeroPacket(441, 30000000 + index * 100000);
    burst_packet.position = index * 441;
    burst->Push(std::move(burst_packet));
  }
  WaitUntil([&] { return burst_notifications.load() == 8; },
            "The worker must capture the complete scheduling burst");
  for (unsigned index = 0; index < 8; ++index) {
    auto burst_packet = burst_capture.Pop();
    Require(
        burst_packet && burst_packet->device_frame_position == index * 441 &&
            !(burst_packet->flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) &&
            burst_packet->owned_bytes == NonzeroPacket(441).bytes,
        "A scheduling burst preserves every PCM sample in order");
  }
  Require(!burst_capture.Pop() && burst_capture.DroppedBlocks() == 0,
          "Low latency capacity must not manufacture capture losses");
  burst_capture.Stop();
}
