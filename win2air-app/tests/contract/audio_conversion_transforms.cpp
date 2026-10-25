#include "audio_conversion_support.h"

using namespace Win2Air;
using namespace Win2Air::Tests;
void TestTransformBackpressure() {
  auto transform = winrt::make_self<Tests::TransformFixture>();
  FormatConverter converter([&] { return transform.as<IMFTransform>(); });
  auto format = InputFormat(WAVE_FORMAT_PCM, 44100, 2, 16);
  converter.Open(format, 1);
  auto input = InputBlock(format, 1, 10000000, 441);
  transform->pending_output.push_back(std::vector<uint8_t>(8, 42));
  transform->reject_next_input = true;
  auto output = converter.Convert(input);
  Require(transform->attempted_samples.size() == 2 &&
              transform->attempted_samples[0].get() ==
                  transform->attempted_samples[1].get() &&
              transform->accepted_bytes.size() == 1 &&
              transform->accepted_bytes[0] == input.owned_bytes &&
              transform->accepted_times[0] == 0 &&
              transform->accepted_durations[0] == 100000,
          "MFT backpressure pulls pending output and retries the identical "
          "sample without duplication");
  Require(
      output.size() == 2 && output[0].pcm == std::vector<uint8_t>(8, 42) &&
          output[1].pcm == input.owned_bytes && output[0].first_sample == 0 &&
          output[1].first_sample == 2,
      "Pending and newly accepted MFT output retain ordered complete frames");
  input.capture_100ns += 100000;
  converter.Convert(input);
  Require(output[0].pcm == std::vector<uint8_t>(8, 42) &&
              output[1].pcm == input.owned_bytes,
          "Reusing transform scratch storage preserves previously returned "
          "owned PCM output");
  Require(transform->accepted_times.back() == 100000,
          "Retried MFT sample advances input time once");

  transform->pending_output.push_back(std::vector<uint8_t>(12, 77));
  converter.Flush(2);
  Require(
      converter.Drain().empty() && transform->pending_output.empty(),
      "Flush discards buffered tail audio before establishing a new anchor");
  auto const& messages = transform->messages;
  Require(
      messages.size() >= 3 &&
          messages[messages.size() - 3] == MFT_MESSAGE_COMMAND_FLUSH &&
          messages[messages.size() - 2] == MFT_MESSAGE_NOTIFY_BEGIN_STREAMING &&
          messages.back() == MFT_MESSAGE_NOTIFY_START_OF_STREAM,
      "Flush restarts the transform before new source audio");
  auto attempts = transform->attempted_samples.size();
  Rejects([&] { converter.Convert(input); },
          "Flushed source epoch cannot reach the transform");
  Require(transform->attempted_samples.size() == attempts,
          "Old epoch input is rejected before ProcessInput");
  input.source_epoch = 2;
  input.capture_100ns = 30000000;
  output = converter.Convert(input);
  Require(output.size() == 1 && output[0].source_epoch == 2 &&
              output[0].timeline_epoch == 2 && output[0].first_sample == 0 &&
              output[0].capture_100ns == input.capture_100ns &&
              output[0].pcm == input.owned_bytes,
          "New source starts an independent timeline without buffered tail");

  input.capture_100ns += 400000;
  bool discontinuity = false;
  attempts = transform->attempted_samples.size();
  try {
    converter.Convert(input);
  } catch (SourceDiscontinuity const&) {
    discontinuity = true;
  }
  Require(discontinuity && transform->attempted_samples.size() == attempts,
          "Source drift requires reanchor before accepting audio");
  converter.Flush(2);
  output = converter.Convert(input);
  Require(output.size() == 1 && output[0].timeline_epoch == 3 &&
              output[0].first_sample == 0 &&
              output[0].capture_100ns == input.capture_100ns,
          "Flush establishes the measured capture timestamp after drift");

  converter.Flush(3);
  input.source_epoch = 3;
  transform->output_failure = E_ACCESSDENIED;
  bool output_error = false;
  try {
    converter.Convert(input);
  } catch (winrt::hresult_error const& error) {
    output_error = error.code() == E_ACCESSDENIED;
  }
  Require(output_error, "ProcessOutput failure preserves its HRESULT");
  transform->output_failure = S_OK;
  converter.Flush(4);
  input.source_epoch = 4;
  transform->emit_input = false;
  transform->pending_output.push_back(std::vector<uint8_t>(3, 1));
  Rejects([&] { converter.Convert(input); },
          "Partial stereo frames from a transform are rejected");
  converter.Flush(5);
  input.source_epoch = 5;
  transform->pending_output.push_back({});
  transform->pending_output.push_back(std::vector<uint8_t>(4, 8));
  output = converter.Convert(input);
  Require(output.size() == 1 && output[0].frame_count == 1,
          "Empty output does not hide the following complete output frame");

  input.format.channel_mask = 4;
  Rejects([&] { converter.Convert(input); },
          "Capture channel mapping must match the negotiated transform format");
  input.format.channel_mask = 3;
  input.owned_bytes.pop_back();
  Rejects([&] { converter.Convert(input); },
          "Truncated captured frame is rejected before transform submission");
  input.owned_bytes.push_back(1);
  input.flags = AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR;
  Rejects([&] { converter.Convert(input); },
          "Unreliable capture timestamp requires a source reanchor");
  input.flags = 0;
  input.capture_100ns = 0;
  Rejects([&] { converter.Convert(input); },
          "Missing capture timestamp cannot establish a transform timeline");
  FormatConverter empty_factory([] { return winrt::com_ptr<IMFTransform>{}; });
  Rejects(
      [&] { empty_factory.Open(format, 1); },
      "Unavailable transform factory fails during converter initialization");
}

void TestSoftwareFormatMatrix() {
  auto run = [](WAVEFORMATEX const& format) {
    FormatConverter converter;
    converter.Open(format, 1);
    auto input = InputBlock(format, 1, 10000000, format.nSamplesPerSec / 10);
    auto output = converter.Convert(input);
    auto tail = converter.Drain();
    output.insert(output.end(), tail.begin(), tail.end());
    uint64_t frames = 0;
    bool audible = false;
    for (auto const& block : output) {
      Require(
          block.first_sample == frames &&
              block.pcm.size() == size_t(block.frame_count) * 4 &&
              block.source_epoch == 1 && block.timeline_epoch == 1 &&
              block.capture_100ns == 10000000 + frames * 10000000 / 44100,
          "Software resampler preserves complete frames and capture timeline");
      frames += block.frame_count;
      for (auto byte : block.pcm) audible |= byte != 0;
    }
    Require(frames >= 4000 && frames <= 4500 && audible,
            "Software resampler preserves nonzero audio and duration");
  };
  run(InputFormat(WAVE_FORMAT_PCM, 8000, 1, 16));
  run(InputFormat(WAVE_FORMAT_PCM, 48000, 2, 24));
  run(InputFormat(WAVE_FORMAT_PCM, 96000, 2, 32));
  run(InputFormat(WAVE_FORMAT_IEEE_FLOAT, 192000, 2, 32));
  WAVEFORMATEXTENSIBLE surround{};
  surround.Format = InputFormat(WAVE_FORMAT_EXTENSIBLE, 48000, 6, 16);
  surround.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  surround.Samples.wValidBitsPerSample = 16;
  surround.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                           SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                           SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
  surround.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
  run(surround.Format);

  FormatConverter converter;
  auto invalid = InputFormat(WAVE_FORMAT_PCM, 7999, 2, 16);
  Rejects([&] { converter.Open(invalid, 1); },
          "Unsupported sample rate rejected");
  invalid = InputFormat(WAVE_FORMAT_IEEE_FLOAT, 48000, 2, 16);
  Rejects([&] { converter.Open(invalid, 1); },
          "Invalid float container rejected");
  invalid = InputFormat(WAVE_FORMAT_PCM, 48000, 3, 16);
  Rejects([&] { converter.Open(invalid, 1); },
          "Multichannel audio requires an explicit speaker mapping");
  invalid = InputFormat(WAVE_FORMAT_PCM, 48000, 2, 16);
  invalid.nBlockAlign = 3;
  Rejects([&] { converter.Open(invalid, 1); },
          "Incomplete input frame rejected");
  surround.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  Rejects([&] { converter.Open(surround.Format, 1); },
          "Extensible speaker mapping must describe every input channel");
  surround.Format.cbSize = 0;
  Rejects([&] { converter.Open(surround.Format, 1); },
          "Truncated extensible format rejected before transform creation");
}

void TestDirectPcm() {
  auto format = InputFormat(WAVE_FORMAT_PCM, 44100, 2, 16);
  FormatConverter converter;
  converter.Open(format, 1);
  uint64_t frames{};
  for (auto count : {1u, 351u, 352u, 441u}) {
    auto input =
        InputBlock(format, 1, 10000000 + frames * 10000000 / 44100, count);
    for (size_t index = 0; index < input.owned_bytes.size(); ++index)
      input.owned_bytes[index] = static_cast<uint8_t>(index);
    auto output = converter.Convert(input);
    Require(output.size() == 1 && output[0].pcm == input.owned_bytes &&
                output[0].first_sample == frames &&
                output[0].frame_count == count &&
                output[0].capture_100ns == input.capture_100ns,
            "Protocol-ready PCM leaves immediately without lookahead, "
            "quantization or frame loss");
    input.owned_bytes.assign(input.owned_bytes.size(), 0);
    Require(
        output[0].pcm.back() == static_cast<uint8_t>(output[0].pcm.size() - 1),
        "Direct PCM owns its bytes after the capture buffer is reused");
    frames += count;
  }
  Require(converter.Drain().empty(), "Direct PCM keeps no delayed tail");
  converter.Flush(2);
  auto input = InputBlock(format, 2, 20000000, 1);
  auto output = converter.Convert(input);
  Require(output.size() == 1 && output[0].first_sample == 0 &&
              output[0].timeline_epoch == 2 &&
              output[0].capture_100ns == 20000000,
          "Direct PCM reanchors after FLUSH without old samples");
  input.source_epoch = 1;
  Rejects([&] { converter.Convert(input); },
          "Direct PCM rejects audio from a previous source epoch");
  converter.Open(InputFormat(WAVE_FORMAT_IEEE_FLOAT, 48000, 2, 32), 3);
  converter.Open(format, 4);
  input = InputBlock(format, 4, 30000000, 352);
  output = converter.Convert(input);
  Require(output.size() == 1 && output[0].pcm == input.owned_bytes &&
              output[0].first_sample == 0 && output[0].timeline_epoch == 1,
          "Changing from resampling to direct PCM discards transform state");

  auto measure = [&](bool direct) {
    FormatConverter::TransformFactory factory;
    if (!direct)
      factory = [] {
        winrt::com_ptr<IMFTransform> transform;
        winrt::check_hresult(CoCreateInstance(
            __uuidof(CResamplerMediaObject), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IMFTransform), transform.put_void()));
        return transform;
      };
    FormatConverter measured(factory);
    measured.Open(format, 1);
    auto block = InputBlock(format, 1, 10000000, 441);
    uint64_t delivered{}, delayed{};
    auto start = std::chrono::steady_clock::now();
    for (uint64_t index = 0; index < 2000; ++index) {
      block.capture_100ns = 10000000 + index * 100000;
      for (auto const& current : measured.Convert(block))
        delivered += current.frame_count;
      delayed = std::max(delayed, (index + 1) * 441 - delivered);
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);
    for (auto const& tail : measured.Drain()) delivered += tail.frame_count;
    Require(delivered == 2000 * 441,
            "Continuous conversion and drain preserve every input frame");
    if (direct)
      Require(delayed == 0, "Direct PCM never waits for following capture");
    std::cout << (direct ? "direct-pcm" : "native-mft-baseline")
              << " processing-us=" << elapsed.count()
              << " maximum-delayed-frames=" << delayed << '\n';
  };
  measure(false);
  measure(true);
}
