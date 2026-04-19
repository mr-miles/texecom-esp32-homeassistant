#pragma once

// Header-only fixed-capacity ring buffer (SPSC).
//
// Deliberately single-producer / single-consumer: the producer is the
// AsyncTCP / UART callback and the consumer is the cooperative `loop()`
// (or vice versa). No locking, no exceptions, no heap allocation.
//
// Capacity is a compile-time power-of-two-or-not template parameter `N`;
// storage lives inside the struct so the caller decides placement
// (stack / static / member) with zero dynamic allocation.

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace texecom {

template <typename T, std::size_t N>
class RingBuffer {
  static_assert(N > 0, "RingBuffer capacity must be > 0");

 public:
  RingBuffer() = default;

  // Non-copyable, non-movable — callers hold pointers/references.
  RingBuffer(const RingBuffer &) = delete;
  RingBuffer &operator=(const RingBuffer &) = delete;

  // Push one value. Returns false if the buffer is full (caller decides
  // whether to drop-oldest, drop-newest, or apply backpressure).
  bool push(const T &v) {
    if (full()) {
      return false;
    }
    data_[head_] = v;
    head_ = advance_(head_);
    ++size_;
    return true;
  }

  // Pop one value into `out`. Returns false if empty.
  bool pop(T &out) {
    if (empty()) {
      return false;
    }
    out = data_[tail_];
    tail_ = advance_(tail_);
    --size_;
    return true;
  }

  // Peek at the next value without consuming.
  bool peek(T &out) const {
    if (empty()) {
      return false;
    }
    out = data_[tail_];
    return true;
  }

  // Drop the oldest element (for drop-oldest overflow policy).
  bool drop_oldest() {
    if (empty()) {
      return false;
    }
    tail_ = advance_(tail_);
    --size_;
    return true;
  }

  // Force-push: drop oldest if full, then push. Never fails.
  void push_overwrite(const T &v) {
    if (full()) {
      drop_oldest();
    }
    (void) push(v);
  }

  void clear() {
    head_ = 0;
    tail_ = 0;
    size_ = 0;
  }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return N; }
  std::size_t free_space() const { return N - size_; }
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == N; }

 private:
  static std::size_t advance_(std::size_t i) { return (i + 1) % N; }

  std::array<T, N> data_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
};

}  // namespace texecom
}  // namespace esphome
