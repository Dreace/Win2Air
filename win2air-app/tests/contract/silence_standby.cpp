#include <cstring>
#include "../../src/Audio/SilenceDetector.h"
#include "test_support.h"
using namespace Win2Air;
namespace {
ConvertedBlock Tone(unsigned frames, int16_t value, uint64_t first = 0) {
  ConvertedBlock block;
  block.source_epoch = 1;
  block.frame_count = frames;
  block.first_sample = first;
  block.pcm.resize(size_t(frames) * 4);
  for (size_t offset = 0; offset < block.pcm.size(); offset += 2)
    std::memcpy(block.pcm.data() + offset, &value, 2);
  return block;
}
}  // namespace
void TestSilenceStandby() {
  Require(std::wstring_view(ConnectedStatusKey(
              SessionState::streaming, StandbyState::silence_pending)) ==
              L"StatusPlaying",
          "Quiet windows retain playing status until standby is confirmed");
  constexpr uint64_t second = 1000000000;
  SilenceDetector detector;
  for (int seconds : {10, 30, 60}) {
    detector.Configure(seconds, second);
    Require(!detector.Due(second + uint64_t(seconds) * second - 1) &&
                detector.Due(second + uint64_t(seconds) * second),
            "No-buffer timer advances the selected silence timeout exactly");
  }
  detector.Configure(0, second);
  Require(!detector.Due(1000 * second), "Never keeps a silent source active");
  Rejects([&] { detector.Configure(15, second); },
          "Unsupported silence choice is rejected");
  detector.Configure(10, second);
  Require(!detector.Consume(Tone(441, 8), second),
          "A voiced window alone never wakes standby");
  Require(detector.Consume(Tone(441, 8, 441), second),
          "Adjacent voiced windows retain low-level valid content");
  detector.Discontinuity(second);
  Require(!detector.Consume(Tone(441, 8), second) &&
              !detector.Consume(Tone(441, 0, 441), second) &&
              !detector.Consume(Tone(441, 8, 882), second),
          "Nonadjacent voiced windows do not accumulate");
  detector.Discontinuity(second);
  Require(!detector.Consume(Tone(220, 4096), second) &&
              !detector.Consume(Tone(221, 4096, 220), second) &&
              detector.Consume(Tone(441, 4096, 441), second),
          "Incomplete PCM windows survive buffer boundaries");
  detector.Discontinuity(second);
  Require(!detector.Consume(Tone(441, 4096), second) &&
              !detector.Consume(Tone(441, 4096, 882), second),
          "Sample gaps reset voiced adjacency");
  detector.Discontinuity(second);
  auto pulse = Tone(882, 0);
  auto single_channel = Tone(882, 8);
  int16_t zero{};
  for (size_t offset = 0; offset < single_channel.pcm.size(); offset += 4)
    std::memcpy(single_channel.pcm.data() + offset, &zero, 2);
  Require(detector.Consume(single_channel, second),
          "A quiet voiced channel wakes using the maximum channel RMS, rather "
          "than a stereo average");
  detector.Discontinuity(second);
  int16_t high = 4096;
  for (size_t frame = 220; frame < 661; ++frame)
    std::memcpy(pulse.pcm.data() + frame * 4, &high, 2);
  Require(detector.Consume(pulse, second),
          "A pulse spanning adjacent windows follows actual channel RMS");
  detector.Configure(10, second);
  Require(!detector.Consume(Tone(882, 2), second + second) &&
              detector.Due(11 * second),
          "Content below the silence threshold keeps the timeout advancing");
  detector.Configure(10, second);
  Require(!detector.Consume(Tone(882, 4), 5 * second) &&
              !detector.Due(11 * second) && detector.Due(15 * second),
          "Threshold gap prevents silence while requiring stronger content to "
          "wake an already sleeping session");
  auto malformed = Tone(441, 0);
  malformed.pcm.pop_back();
  Rejects([&] { detector.Consume(malformed, second); },
          "Malformed PCM is a source error rather than silence");
}
