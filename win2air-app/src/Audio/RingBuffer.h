#pragma once
#include <deque>
#include <stdexcept>

#include "../Streaming/SessionModels.h"
namespace Win2Air {
class AudioRingBuffer {
  size_t capacity_, bytes_{};
  std::deque<ConvertedBlock> blocks_;

 public:
  explicit AudioRingBuffer(size_t capacity) : capacity_(capacity) {
    if (!capacity || capacity % 4)
      throw std::runtime_error(
          "PCM queue capacity must contain complete frames");
  }
  bool Push(ConvertedBlock block);
  void SetCapacity(size_t capacity) {
    if (!capacity || capacity % 4 || bytes_)
      throw std::runtime_error(
          "PCM capacity changes require an empty queue and complete frames");
    capacity_ = capacity;
  }
  std::optional<ConvertedBlock> Pop();
  ConvertedBlock const* Front() const noexcept {
    return blocks_.empty() ? nullptr : &blocks_.front();
  }
  void Clear() noexcept {
    blocks_.clear();
    bytes_ = 0;
  }
  size_t Bytes() const noexcept { return bytes_; }
  size_t Capacity() const noexcept { return capacity_; }
};
}  // namespace Win2Air
