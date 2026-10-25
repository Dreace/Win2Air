#include "RingBuffer.h"
namespace Win2Air {
bool AudioRingBuffer::Push(ConvertedBlock block) {
  if (!block.frame_count || block.pcm.size() != size_t(block.frame_count) * 4 ||
      !block.source_epoch || !block.timeline_epoch)
    throw std::runtime_error("PCM block shape or timeline is invalid");
  if (block.pcm.size() > capacity_ - bytes_) return false;
  auto size = block.pcm.size();
  blocks_.push_back(std::move(block));
  bytes_ += size;
  return true;
}
std::optional<ConvertedBlock> AudioRingBuffer::Pop() {
  if (blocks_.empty()) return {};
  auto block = std::move(blocks_.front());
  blocks_.pop_front();
  bytes_ -= block.pcm.size();
  return block;
}
}  // namespace Win2Air
