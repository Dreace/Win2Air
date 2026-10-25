#pragma once

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <winrt/base.h>

#include <cstring>
#include <deque>
#include <vector>

namespace Win2Air::Tests {
class TransformFixture
    : public winrt::implements<TransformFixture, IMFTransform> {
 public:
  std::deque<std::vector<uint8_t>> pending_output;
  std::vector<winrt::com_ptr<IMFSample>> attempted_samples;
  std::vector<std::vector<uint8_t>> accepted_bytes;
  std::vector<LONGLONG> accepted_times;
  std::vector<LONGLONG> accepted_durations;
  std::vector<MFT_MESSAGE_TYPE> messages;
  HRESULT output_failure = S_OK;
  bool reject_next_input = false;
  bool emit_input = true;
  unsigned batch_input_count{1};
  std::vector<uint8_t> batched_input;

  HRESULT __stdcall GetStreamLimits(DWORD*, DWORD*, DWORD*,
                                    DWORD*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetStreamCount(DWORD*, DWORD*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetStreamIDs(DWORD, DWORD*, DWORD,
                                 DWORD*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetInputStreamInfo(
      DWORD, MFT_INPUT_STREAM_INFO*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetOutputStreamInfo(
      DWORD stream, MFT_OUTPUT_STREAM_INFO* info) noexcept override {
    if (stream || !info) return E_INVALIDARG;
    *info = {};
    info->cbSize = 17640;
    return S_OK;
  }
  HRESULT __stdcall GetAttributes(IMFAttributes**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetInputStreamAttributes(
      DWORD, IMFAttributes**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetOutputStreamAttributes(
      DWORD, IMFAttributes**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall DeleteInputStream(DWORD) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall AddInputStreams(DWORD, DWORD*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetInputAvailableType(DWORD, DWORD,
                                          IMFMediaType**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetOutputAvailableType(DWORD, DWORD,
                                           IMFMediaType**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall SetInputType(DWORD stream, IMFMediaType* type,
                                 DWORD flags) noexcept override {
    return stream || !type || flags ? E_INVALIDARG : S_OK;
  }
  HRESULT __stdcall SetOutputType(DWORD stream, IMFMediaType* type,
                                  DWORD flags) noexcept override {
    return stream || !type || flags ? E_INVALIDARG : S_OK;
  }
  HRESULT __stdcall GetInputCurrentType(DWORD,
                                        IMFMediaType**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetOutputCurrentType(DWORD,
                                         IMFMediaType**) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetInputStatus(DWORD, DWORD*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall GetOutputStatus(DWORD*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall SetOutputBounds(LONGLONG, LONGLONG) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall ProcessEvent(DWORD, IMFMediaEvent*) noexcept override {
    return E_NOTIMPL;
  }
  HRESULT __stdcall ProcessMessage(MFT_MESSAGE_TYPE message,
                                   ULONG_PTR) noexcept override {
    try {
      messages.push_back(message);
      if (message == MFT_MESSAGE_COMMAND_FLUSH) {
        pending_output.clear();
        batched_input.clear();
      }
      return S_OK;
    } catch (...) {
      return winrt::to_hresult();
    }
  }
  HRESULT __stdcall ProcessInput(DWORD stream, IMFSample* sample,
                                 DWORD flags) noexcept override {
    if (stream || !sample || flags) return E_INVALIDARG;
    try {
      winrt::com_ptr<IMFSample> retained;
      retained.copy_from(sample);
      attempted_samples.push_back(std::move(retained));
      if (reject_next_input) {
        reject_next_input = false;
        return MF_E_NOTACCEPTING;
      }
      winrt::com_ptr<IMFMediaBuffer> buffer;
      winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
      DWORD size{};
      winrt::check_hresult(buffer->GetCurrentLength(&size));
      std::vector<uint8_t> bytes(size);
      BYTE* data{};
      winrt::check_hresult(buffer->Lock(&data, nullptr, nullptr));
      std::memcpy(bytes.data(), data, size);
      winrt::check_hresult(buffer->Unlock());
      LONGLONG time{}, duration{};
      winrt::check_hresult(sample->GetSampleTime(&time));
      winrt::check_hresult(sample->GetSampleDuration(&duration));
      accepted_bytes.push_back(bytes);
      accepted_times.push_back(time);
      accepted_durations.push_back(duration);
      if (emit_input) {
        batched_input.insert(batched_input.end(), bytes.begin(), bytes.end());
        if (accepted_bytes.size() % batch_input_count == 0) {
          pending_output.push_back(std::move(batched_input));
          batched_input.clear();
        }
      }
      return S_OK;
    } catch (...) {
      return winrt::to_hresult();
    }
  }
  HRESULT __stdcall ProcessOutput(DWORD flags, DWORD count,
                                  MFT_OUTPUT_DATA_BUFFER* output,
                                  DWORD* status) noexcept override {
    if (flags || count != 1 || !output || !output->pSample || !status)
      return E_INVALIDARG;
    if (FAILED(output_failure)) return output_failure;
    if (pending_output.empty()) return MF_E_TRANSFORM_NEED_MORE_INPUT;
    try {
      winrt::com_ptr<IMFMediaBuffer> buffer;
      winrt::check_hresult(output->pSample->GetBufferByIndex(0, buffer.put()));
      DWORD capacity{};
      winrt::check_hresult(buffer->GetMaxLength(&capacity));
      auto const& bytes = pending_output.front();
      if (bytes.size() > capacity) return MF_E_BUFFERTOOSMALL;
      BYTE* data{};
      winrt::check_hresult(buffer->Lock(&data, nullptr, nullptr));
      std::memcpy(data, bytes.data(), bytes.size());
      winrt::check_hresult(buffer->Unlock());
      winrt::check_hresult(
          buffer->SetCurrentLength(static_cast<DWORD>(bytes.size())));
      pending_output.pop_front();
      *status = 0;
      return S_OK;
    } catch (...) {
      return winrt::to_hresult();
    }
  }
};
}  // namespace Win2Air::Tests
