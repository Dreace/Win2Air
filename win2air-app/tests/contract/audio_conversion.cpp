#include "audio_conversion_support.h"

using namespace Win2Air;
using namespace Win2Air::Tests;
void TestDevicePositionContinuity() {
  FormatConverter converter;
  auto format = InputFormat(WAVE_FORMAT_PCM, 44100, 2, 16);
  converter.Open(format, 1);
  auto first = InputBlock(format, 1, 10000000, 441);
  first.device_frame_position = 1000;
  auto output = converter.Convert(first);
  Require(output.size() == 1 && output[0].timeline_epoch == 1 &&
              output[0].first_sample == 0 &&
              output[0].capture_100ns == first.capture_100ns,
          "The first device position anchors the capture clock");
  auto jumped = InputBlock(format, 1, first.capture_100ns + 400000, 441);
  jumped.device_frame_position = 1441;
  output = converter.Convert(jumped);
  Require(output.size() == 1 && output[0].timeline_epoch == 1 &&
              output[0].first_sample == 441 &&
              output[0].capture_100ns == first.capture_100ns + 100000,
          "A continuous device position keeps the sample clock across a "
          "capture timestamp jump");
  auto unreliable = InputBlock(format, 1, 0, 441);
  unreliable.flags = AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
  unreliable.device_frame_position = 1882;
  output = converter.Convert(unreliable);
  Require(output.size() == 1 && output[0].timeline_epoch == 1 &&
              output[0].first_sample == 882 &&
              output[0].capture_100ns == first.capture_100ns + 200000,
          "A timestamp error on continuous device frames stays on the same "
          "timeline");
  auto gap = InputBlock(format, 1, output[0].capture_100ns + 400000, 441);
  gap.device_frame_position = 3000;
  bool discontinuity = false;
  try {
    converter.Convert(gap);
  } catch (SourceDiscontinuity const&) {
    discontinuity = true;
  }
  Require(discontinuity,
          "A device position gap with a late timestamp still requires FLUSH");
  converter.Flush(1);
  gap.capture_100ns = 30000000;
  output = converter.Convert(gap);
  Require(
      output.size() == 1 && output[0].timeline_epoch == 2 &&
          output[0].first_sample == 0 &&
          output[0].capture_100ns == gap.capture_100ns,
      "Flush anchors the next measured timestamp after a real position gap");
}

void TestAudioConversion() {
  {
    auto format = InputFormat(WAVE_FORMAT_IEEE_FLOAT, 48000, 2, 32);
    FormatConverter converter;
    converter.Open(format, 1);
    uint64_t captured{}, delivered{};
    int previous{};
    unsigned largest_step{};
    for (unsigned index = 0; index < 12000; ++index) {
      auto frames = 432 + (index % 3) * 48;
      auto block =
          InputBlock(format, 1, 10000000 + captured * 10000000 / 48000, frames);
      block.device_frame_position = captured;
      for (unsigned frame = 0; frame < frames; ++frame) {
        auto value =
            float(0.25 * std::sin(2.0 * 3.14159265358979323846 * 440.0 *
                                  double(captured + frame) / 48000));
        std::memcpy(block.owned_bytes.data() + size_t(frame) * 8, &value, 4);
        std::memcpy(block.owned_bytes.data() + size_t(frame) * 8 + 4, &value,
                    4);
      }
      captured += frames;
      for (auto const& output : converter.Convert(block)) {
        Require(
            output.first_sample == delivered,
            "Long resampling preserves the continuous output sample position");
        for (unsigned frame = 0; frame < output.frame_count; ++frame) {
          int16_t sample;
          std::memcpy(&sample, output.pcm.data() + size_t(frame) * 4, 2);
          if (delivered > 512)
            largest_step = std::max(largest_step,
                                    unsigned(std::abs(int(sample) - previous)));
          previous = sample;
          ++delivered;
        }
      }
    }
    std::cout << "resampled-120s frames=" << delivered
              << " maximum-sample-step=" << largest_step << '\n';
    Require(delivered > 5290000 && delivered <= 5292000 && largest_step < 600,
            "A continuous 440 Hz waveform remains smooth across variable "
            "capture and output block boundaries");
  }
  {
    uint64_t now = 1000000000;
    FormatConverter observed({}, [&] { return now; });
    auto format = InputFormat(WAVE_FORMAT_PCM, 44100, 2, 16);
    observed.Open(format, 1);
    auto block = InputBlock(format, 1, 10000000, 441);
    observed.Convert(block);
    block.device_frame_position = 441;
    block.capture_100ns += 160000;
    now += 10000000;
    observed.Convert(block);
    block.device_frame_position += 441;
    block.capture_100ns += 100000;
    now += 3000000000;
    auto corrected = observed.Convert(block);
    Require(corrected.front().capture_100ns == block.capture_100ns &&
                corrected.front().first_sample == 882 &&
                corrected.front().pcm == block.owned_bytes,
            "Sustained clock correction retains PCM order and anchors to the "
            "measured capture time");
    block.device_frame_position += 441;
    block.capture_100ns += 1100000;
    auto ignored = observed.Convert(block);
    Require(ignored.front().capture_100ns ==
                    corrected.front().capture_100ns + 100000 &&
                ignored.front().first_sample == 1323 &&
                ignored.front().pcm == block.owned_bytes,
            "Continuous device samples retain their clock across a capture "
            "timestamp jump");
    block.device_frame_position = 0;
    block.capture_100ns += 100000;
    Rejects([&] { observed.Convert(block); },
            "A noncontinuous source clock jump still requests a flush");
  }
  for (auto format : {InputFormat(WAVE_FORMAT_PCM, 44100, 2, 16),
                      InputFormat(WAVE_FORMAT_IEEE_FLOAT, 48000, 2, 32)}) {
    FormatConverter converter;
    converter.Open(format, 1);
    uint64_t position = 1000, elapsed{}, received{};
    bool sound{};
    auto consume = [&](std::vector<ConvertedBlock> const& output) {
      for (auto const& block : output) {
        Require(block.timeline_epoch == 1 && block.first_sample == received,
                "Bounded capture loss retains the receiver sample timeline");
        received += block.frame_count;
        sound |= std::any_of(block.pcm.begin(), block.pcm.end(),
                             [](uint8_t value) { return value != 0; });
      }
    };
    for (unsigned index = 0; index < 1000; ++index) {
      auto frames = format.nSamplesPerSec / 100;
      bool gap = index && index % 5 == 0;
      if (gap) {
        position += frames;
        elapsed += frames;
      }
      auto input = InputBlock(
          format, 1, 10000000 + elapsed * 10000000 / format.nSamplesPerSec,
          frames);
      input.device_frame_position = position;
      if (gap) input.flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
      consume(converter.Convert(input));
      position += frames;
      elapsed += frames;
    }
    consume(converter.Drain());
    auto expected = elapsed * 44100 / format.nSamplesPerSec;
    Require(received >= expected - 1 && received <= expected + 1 && sound,
            "Repeated dropped capture packets preserve elapsed audio time "
            "through PCM and resampling without a stream reset");
  }
  TestDevicePositionContinuity();
  TestTransformBackpressure();
  TestSoftwareFormatMatrix();
  TestDirectPcm();
  AudioRingBuffer queue(1764);
  ConvertedBlock block;
  block.source_epoch = 1;
  block.timeline_epoch = 1;
  block.frame_count = 441;
  block.pcm.resize(1764, 1);
  Require(queue.Push(block) && !queue.Push(block),
          "PCM queue accepts complete blocks or zero bytes");
  auto popped = queue.Pop();
  Require(popped && popped->pcm.size() == 1764,
          "PCM queue preserves complete block");
  ConvertedBlock owned = block;
  auto storage = owned.pcm.data();
  Require(queue.Push(std::move(owned)) && queue.Front()->pcm.data() == storage,
          "Streaming transfers ownership without copying the PCM payload");
  queue.Clear();
  queue.Push(block);
  Require(CaptureTime(1010000000, 1000000000, 1000000000) ==
                  CaptureReadiness::wait &&
              queue.Front()->pcm.size() == 1764,
          "Future capture remains a complete queued block");
  Require(CaptureTime(1010000000, 1010000000, 1000000000) ==
              CaptureReadiness::ready,
          "Capture becomes eligible when the source timestamp arrives");
  Require(CaptureTime(1000000000, 2000000001, 1000000000) ==
              CaptureReadiness::expired,
          "Old capture cannot consume a fresh timeline");
  queue.Clear();
  AudioClock clock;
  clock.Begin(1, 1000000000);
  Require(clock.Stamp(441) == 1010000000, "Integer sample clock mapping");
  Require(clock.Observe(441, 1017000000, 1000000000) == ClockAction::stable,
          "Clock drift requires continuous evidence");
  Require(clock.Observe(441, 1017000000, 4000000000) ==
                  ClockAction::correct_anchor &&
              clock.Stamp(441) == 1017000000,
          "Sustained drift corrects the integer source anchor");
  Require(clock.Observe(441, 1040000000, 4100000000) == ClockAction::reanchor,
          "Large source drift requires explicit FLUSH");
  clock.Begin(2, UINT64_MAX - 100);
  Rejects([&] { clock.Stamp(1); },
          "Subsecond sample fraction overflow rejected");
  FormatConverter converter;
  WAVEFORMATEX input{};
  input.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  input.nSamplesPerSec = 48000;
  input.wBitsPerSample = 32;
  input.nChannels = 2;
  input.nBlockAlign = 8;
  input.nAvgBytesPerSec = 48000 * 8;
  converter.Open(input, 1);
  CapturedBlock captured;
  captured.source_epoch = 1;
  captured.frame_count = 4800;
  captured.capture_100ns = 10000000;
  captured.format = {48000, 2, 32, 3};
  captured.owned_bytes.resize(4800 * 8);
  float sample = 0.25f;
  for (size_t offset = 0; offset < captured.owned_bytes.size(); offset += 4)
    std::memcpy(captured.owned_bytes.data() + offset, &sample, 4);
  auto output = converter.Convert(captured);
  auto tail = converter.Drain();
  output.insert(output.end(), tail.begin(), tail.end());
  uint64_t next{};
  size_t frames{};
  bool sound = false;
  for (auto const& converted : output) {
    Require(converted.first_sample == next &&
                converted.pcm.size() == converted.frame_count * 4,
            "MFT output frames and timeline stay aligned");
    next += converted.frame_count;
    frames += converted.frame_count;
    for (auto value : converted.pcm)
      if (value) sound = true;
  }
  Require(frames >= 4000 && frames <= 4500 && sound,
          "Real MFT converts nonzero float audio to fixed PCM");
  converter.Flush(2);
  Rejects([&] { converter.Convert(captured); },
          "Old source rejected after MFT flush");
  WAVEFORMATEX mono{};
  mono.wFormatTag = WAVE_FORMAT_PCM;
  mono.nSamplesPerSec = 44100;
  mono.wBitsPerSample = 16;
  mono.nChannels = 1;
  mono.nBlockAlign = 2;
  mono.nAvgBytesPerSec = 88200;
  converter.Open(mono, 3);
  captured.source_epoch = 3;
  captured.frame_count = 4410;
  captured.format = {44100, 1, 16, 4};
  captured.capture_100ns = 20000000;
  captured.owned_bytes.assign(8820, 1);
  output = converter.Convert(captured);
  tail = converter.Drain();
  output.insert(output.end(), tail.begin(), tail.end());
  frames = 0;
  for (auto const& item : output) {
    Require(item.pcm.size() == item.frame_count * 4,
            "Mono PCM expands to complete stereo frames");
    frames += item.frame_count;
  }
  Require(frames >= 4000 && frames <= 4500,
          "PCM mono conversion preserves duration");
}
