#pragma once
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#include "../../src/Audio/AudioClock.h"
#include "../../src/Audio/FormatConverter.h"
#include "../../src/Audio/RingBuffer.h"

#include <ks.h>
#include <ksmedia.h>
#include <wmcodecdsp.h>

#include "../fixtures/TransformFixture.h"
#include "test_support.h"

// Sample builders shared by the audio conversion contract tests.
namespace Win2Air::Tests {
inline WAVEFORMATEX InputFormat(WORD tag, DWORD rate, WORD channels,
                                WORD bits) {
  WAVEFORMATEX format{};
  format.wFormatTag = tag;
  format.nSamplesPerSec = rate;
  format.nChannels = channels;
  format.wBitsPerSample = bits;
  format.nBlockAlign = channels * bits / 8;
  format.nAvgBytesPerSec = rate * format.nBlockAlign;
  return format;
}
inline CapturedBlock InputBlock(WAVEFORMATEX const& format, uint64_t epoch,
                                uint64_t capture, uint32_t frames) {
  CapturedBlock block;
  block.source_epoch = epoch;
  block.capture_100ns = capture;
  block.frame_count = frames;
  block.format = ValidateAudioFormat(format);
  block.owned_bytes.resize(size_t(frames) * format.nBlockAlign);
  if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    float value = 0.25f;
    for (size_t offset = 0; offset < block.owned_bytes.size(); offset += 4)
      std::memcpy(block.owned_bytes.data() + offset, &value, sizeof(value));
  } else {
    std::fill(block.owned_bytes.begin(), block.owned_bytes.end(), uint8_t{1});
  }
  return block;
}
}  // namespace Win2Air::Tests

// Defined in audio_conversion.cpp / audio_conversion_transforms.cpp; the
// aggregate TestAudioConversion drives them in order.
void TestTransformBackpressure();
void TestSoftwareFormatMatrix();
void TestDirectPcm();
void TestDevicePositionContinuity();
