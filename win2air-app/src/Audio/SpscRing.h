#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace Win2Air {
template <typename T, size_t Capacity>
class SpscRing {
  static_assert(Capacity > 1);
  static_assert(alignof(T) <= 64);
  struct alignas(64) Cursor {
    std::atomic<size_t> value{};
    std::array<std::byte, 64 - sizeof(std::atomic<size_t>)> padding{};
  };
  Cursor write_, read_;
  std::array<std::optional<T>, Capacity> slots_;
  std::array<std::byte, 64 - sizeof(slots_) % 64> padding_{};

 public:
  bool Push(T& value) {
    auto write = write_.value.load(std::memory_order_relaxed);
    auto next = (write + 1) % Capacity;
    if (next == read_.value.load(std::memory_order_acquire)) return false;
    slots_[write].emplace(std::move(value));
    write_.value.store(next, std::memory_order_release);
    return true;
  }
  std::optional<T> Pop() {
    auto read = read_.value.load(std::memory_order_relaxed);
    if (read == write_.value.load(std::memory_order_acquire)) return {};
    auto value = std::move(slots_[read]);
    slots_[read].reset();
    read_.value.store((read + 1) % Capacity, std::memory_order_release);
    return value;
  }
};
}  // namespace Win2Air
