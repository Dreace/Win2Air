#include "FormatConverter.h"

#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
namespace Win2Air {
namespace {
winrt::com_ptr<IMFMediaType> MediaType(AudioFormat const &format,
                                       bool floating) {
  winrt::com_ptr<IMFMediaType> type;
  winrt::check_hresult(MFCreateMediaType(type.put()));
  winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
  winrt::check_hresult(type->SetGUID(
      MF_MT_SUBTYPE, floating ? MFAudioFormat_Float : MFAudioFormat_PCM));
  winrt::check_hresult(
      type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channels));
  winrt::check_hresult(
      type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.rate));
  winrt::check_hresult(
      type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, format.bits));
  winrt::check_hresult(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                       format.channels * format.bits / 8));
  winrt::check_hresult(
      type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                      format.rate * format.channels * format.bits / 8));
  winrt::check_hresult(
      type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, format.channel_mask));
  winrt::check_hresult(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
  return type;
}
}  // namespace
FormatConverter::FormatConverter(TransformFactory factory,
                                 std::function<uint64_t()> now)
    : factory_(std::move(factory)), now_(std::move(now)) {
  if (!now_) throw std::invalid_argument("Audio clock is required");
}
FormatConverter::~FormatConverter() {
  output_buffer_ = nullptr;
  transform_ = nullptr;
  if (started_) MFShutdown();
}
void FormatConverter::Open(WAVEFORMATEX const &format, uint64_t source_epoch) {
  if (!source_epoch)
    throw std::runtime_error("Converter source epoch required");
  input_ = ValidateAudioFormat(format);
  transform_ = nullptr;
  output_buffer_ = nullptr;
  direct_pcm_ =
      !factory_ && input_.rate == 44100 && input_.channels == 2 &&
      input_.bits == 16 && input_.channel_mask == 3 &&
      (format.wFormatTag == WAVE_FORMAT_PCM ||
       (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        reinterpret_cast<WAVEFORMATEXTENSIBLE const &>(format)
                .Samples.wValidBitsPerSample == 16 &&
        reinterpret_cast<WAVEFORMATEXTENSIBLE const &>(format).SubFormat ==
            KSDATAFORMAT_SUBTYPE_PCM));
  if (direct_pcm_) {
    source_epoch_ = source_epoch;
    timeline_epoch_ = 0;
    Flush(source_epoch);
    return;
  }
  if (!started_) {
    winrt::check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    started_ = true;
  }
  transform_ = nullptr;
  if (factory_) {
    transform_ = factory_();
    if (!transform_)
      throw std::runtime_error("Transform factory returned null");
  } else {
    winrt::check_hresult(CoCreateInstance(
        __uuidof(CResamplerMediaObject), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMFTransform), transform_.put_void()));
  }
  bool floating =
      format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
      (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
       reinterpret_cast<WAVEFORMATEXTENSIBLE const &>(format).SubFormat ==
           KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  auto input = MediaType(input_, floating),
       output = MediaType({44100, 2, 16, 3}, false);
  if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    winrt::check_hresult(
        input->SetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE,
                         reinterpret_cast<WAVEFORMATEXTENSIBLE const &>(format)
                             .Samples.wValidBitsPerSample));
  winrt::check_hresult(transform_->SetInputType(0, input.get(), 0));
  winrt::check_hresult(transform_->SetOutputType(0, output.get(), 0));
  winrt::check_hresult(transform_->GetOutputStreamInfo(0, &output_info_));
  winrt::check_hresult(MFCreateAlignedMemoryBuffer(
      std::max<DWORD>(output_info_.cbSize, 17640), output_info_.cbAlignment,
      output_buffer_.put()));
  source_epoch_ = source_epoch;
  timeline_epoch_ = 0;
  Flush(source_epoch);
}
void FormatConverter::Flush(uint64_t source_epoch) {
  if ((!transform_ && !direct_pcm_) || !source_epoch ||
      timeline_epoch_ == UINT64_MAX)
    throw std::runtime_error(
        "Converter requires an open transform and valid source timeline");
  if (transform_) {
    winrt::check_hresult(
        transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
    winrt::check_hresult(
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    winrt::check_hresult(
        transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
  }
  source_epoch_ = source_epoch;
  ++timeline_epoch_;
  next_sample_ = 0;
  input_frames_ = 0;
  anchored_ = false;
  last_device_position_ = 0;
  last_device_frames_ = 0;
}
bool FormatConverter::DevicePositionContinues(
    CapturedBlock const &input) const {
  return anchored_ && last_device_frames_ && input.frame_count &&
         input.device_frame_position ==
             last_device_position_ + last_device_frames_;
}
void FormatConverter::RememberDevicePosition(CapturedBlock const &input) {
  last_device_position_ = input.device_frame_position;
  last_device_frames_ = input.frame_count;
}
std::vector<ConvertedBlock> FormatConverter::Pull() {
  std::vector<ConvertedBlock> result;
  for (;;) {
    winrt::com_ptr<IMFSample> sample;
    winrt::check_hresult(MFCreateSample(sample.put()));
    winrt::check_hresult(output_buffer_->SetCurrentLength(0));
    winrt::check_hresult(sample->AddBuffer(output_buffer_.get()));
    MFT_OUTPUT_DATA_BUFFER data{};
    data.dwStreamID = 0;
    data.pSample = sample.get();
    DWORD status{};
    auto code = transform_->ProcessOutput(0, 1, &data, &status);
    if (data.pEvents) data.pEvents->Release();
    if (code == MF_E_TRANSFORM_NEED_MORE_INPUT) return result;
    winrt::check_hresult(code);
    winrt::com_ptr<IMFMediaBuffer> contiguous;
    winrt::check_hresult(sample->ConvertToContiguousBuffer(contiguous.put()));
    DWORD size{};
    winrt::check_hresult(contiguous->GetCurrentLength(&size));
    if (!size) continue;
    if (size % 4)
      throw std::runtime_error(
          "MFT output did not contain complete stereo PCM frames");
    ConvertedBlock block;
    block.source_epoch = source_epoch_;
    block.timeline_epoch = timeline_epoch_;
    block.first_sample = next_sample_;
    block.capture_100ns = clock_.Stamp(next_sample_) / 100;
    block.frame_count = size / 4;
    BYTE *bytes{};
    winrt::check_hresult(contiguous->Lock(&bytes, nullptr, nullptr));
    try {
      block.pcm.assign(bytes, bytes + size);
    } catch (...) {
      contiguous->Unlock();
      throw;
    }
    winrt::check_hresult(contiguous->Unlock());
    next_sample_ += block.frame_count;
    result.push_back(std::move(block));
  }
}
std::vector<ConvertedBlock> FormatConverter::Convert(
    CapturedBlock const &input) {
  if ((!transform_ && !direct_pcm_) || input.source_epoch != source_epoch_ ||
      input.format.rate != input_.rate ||
      input.format.channels != input_.channels ||
      input.format.bits != input_.bits ||
      input.format.channel_mask != input_.channel_mask ||
      input.owned_bytes.size() !=
          size_t(input.frame_count) * input_.channels * input_.bits / 8 ||
      !input.frame_count || input.owned_bytes.size() > 1024 * 1024)
    throw std::runtime_error(
        "Captured block format, frame count or source epoch does not match the "
        "converter");
  bool timestamp_unusable =
      (input.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ||
      !input.capture_100ns || input.capture_100ns > UINT64_MAX / 100;
  std::vector<ConvertedBlock> result;
  if (anchored_ && last_device_frames_ &&
      (input.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) &&
      last_device_position_ <= UINT64_MAX - last_device_frames_) {
    auto next = last_device_position_ + last_device_frames_;
    if (input.device_frame_position > next &&
        input.device_frame_position - next <= input_.rate / 20) {
      // A bounded capture loss consumes time at the receiver too. Preserve
      // that interval as silence instead of repeatedly flushing its stream.
      CapturedBlock missing;
      missing.source_epoch = input.source_epoch;
      missing.device_frame_position = next;
      missing.frame_count =
          static_cast<uint32_t>(input.device_frame_position - next);
      missing.format = input.format;
      auto sample = input_frames_ / input_.rate * 44100 +
                    input_frames_ % input_.rate * 44100 / input_.rate;
      missing.capture_100ns = clock_.Stamp(sample) / 100;
      missing.flags = AUDCLNT_BUFFERFLAGS_SILENT;
      missing.owned_bytes.resize(size_t(missing.frame_count) * input_.channels *
                                 input_.bits / 8);
      result = Convert(missing);
    }
  }
  bool continuous = DevicePositionContinues(input);
  if (timestamp_unusable && !continuous)
    throw std::runtime_error(
        "Capture timestamp is unavailable; source must reanchor");
  if (!anchored_) {
    clock_.Begin(timeline_epoch_, input.capture_100ns * 100);
    anchored_ = true;
  }
  auto source_sample = input_frames_ / input_.rate * 44100 +
                       input_frames_ % input_.rate * 44100 / input_.rate;
  uint64_t actual = continuous && timestamp_unusable
                        ? clock_.Stamp(source_sample)
                        : input.capture_100ns * 100;
  if (continuous && !timestamp_unusable) {
    auto expected = clock_.Stamp(source_sample);
    auto drift = expected > actual ? expected - actual : actual - expected;
    if (drift > 20000000) {
      actual = expected;
    }
  }
  if (clock_.Observe(source_sample, actual, now_()) == ClockAction::reanchor)
    throw SourceDiscontinuity(
        "Capture source clock discontinuity requires FLUSH");
  if (direct_pcm_) {
    ConvertedBlock block;
    block.source_epoch = source_epoch_;
    block.timeline_epoch = timeline_epoch_;
    block.first_sample = next_sample_;
    block.capture_100ns = clock_.Stamp(next_sample_) / 100;
    block.frame_count = input.frame_count;
    block.pcm = input.owned_bytes;
    input_frames_ += input.frame_count;
    next_sample_ += input.frame_count;
    RememberDevicePosition(input);
    result.push_back(std::move(block));
    return result;
  }
  winrt::com_ptr<IMFSample> sample;
  winrt::com_ptr<IMFMediaBuffer> buffer;
  winrt::check_hresult(MFCreateSample(sample.put()));
  winrt::check_hresult(MFCreateMemoryBuffer(
      static_cast<DWORD>(input.owned_bytes.size()), buffer.put()));
  BYTE *bytes{};
  winrt::check_hresult(buffer->Lock(&bytes, nullptr, nullptr));
  std::memcpy(bytes, input.owned_bytes.data(), input.owned_bytes.size());
  winrt::check_hresult(buffer->Unlock());
  winrt::check_hresult(
      buffer->SetCurrentLength(static_cast<DWORD>(input.owned_bytes.size())));
  winrt::check_hresult(sample->AddBuffer(buffer.get()));
  winrt::check_hresult(sample->SetSampleTime(static_cast<LONGLONG>(
      input_frames_ / input_.rate * 10000000 +
      input_frames_ % input_.rate * 10000000 / input_.rate)));
  winrt::check_hresult(sample->SetSampleDuration(static_cast<LONGLONG>(
      uint64_t(input.frame_count) * 10000000 / input_.rate)));
  auto code = transform_->ProcessInput(0, sample.get(), 0);
  if (code == MF_E_NOTACCEPTING) {
    auto pending = Pull();
    result.insert(result.end(), std::make_move_iterator(pending.begin()),
                  std::make_move_iterator(pending.end()));
    code = transform_->ProcessInput(0, sample.get(), 0);
  }
  winrt::check_hresult(code);
  input_frames_ += input.frame_count;
  RememberDevicePosition(input);
  auto output = Pull();
  result.insert(result.end(), std::make_move_iterator(output.begin()),
                std::make_move_iterator(output.end()));
  return result;
}
std::vector<ConvertedBlock> FormatConverter::Drain() {
  if (!transform_ || !anchored_) return {};
  winrt::check_hresult(
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
  winrt::check_hresult(
      transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
  return Pull();
}
}  // namespace Win2Air
