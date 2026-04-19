// Host-side unit tests for RingBuffer<T, N>.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "ring_buffer.h"

using esphome::texecom::RingBuffer;

TEST_CASE("RingBuffer: empty on construction", "[ring_buffer]") {
  RingBuffer<uint8_t, 8> rb;
  REQUIRE(rb.empty());
  REQUIRE_FALSE(rb.full());
  REQUIRE(rb.size() == 0);
  REQUIRE(rb.capacity() == 8);
  REQUIRE(rb.free_space() == 8);
}

TEST_CASE("RingBuffer: push/pop round-trip preserves values", "[ring_buffer]") {
  RingBuffer<uint8_t, 4> rb;
  REQUIRE(rb.push(0x11));
  REQUIRE(rb.push(0x22));
  REQUIRE(rb.push(0x33));
  REQUIRE(rb.size() == 3);

  uint8_t out = 0;
  REQUIRE(rb.pop(out));
  REQUIRE(out == 0x11);
  REQUIRE(rb.pop(out));
  REQUIRE(out == 0x22);
  REQUIRE(rb.pop(out));
  REQUIRE(out == 0x33);
  REQUIRE(rb.empty());
}

TEST_CASE("RingBuffer: full() true at capacity, push on full fails", "[ring_buffer]") {
  RingBuffer<uint8_t, 3> rb;
  REQUIRE(rb.push(1));
  REQUIRE(rb.push(2));
  REQUIRE(rb.push(3));
  REQUIRE(rb.full());
  REQUIRE(rb.size() == 3);

  REQUIRE_FALSE(rb.push(4));  // push on full must fail
  REQUIRE(rb.size() == 3);    // and NOT modify contents

  // Draining still yields original values in FIFO order.
  uint8_t out = 0;
  REQUIRE(rb.pop(out));
  REQUIRE(out == 1);
  REQUIRE(rb.pop(out));
  REQUIRE(out == 2);
  REQUIRE(rb.pop(out));
  REQUIRE(out == 3);
}

TEST_CASE("RingBuffer: empty() true after drain, pop on empty fails", "[ring_buffer]") {
  RingBuffer<uint8_t, 4> rb;
  REQUIRE(rb.push(42));
  uint8_t out = 0;
  REQUIRE(rb.pop(out));
  REQUIRE(out == 42);
  REQUIRE(rb.empty());

  uint8_t dummy = 0xAA;
  REQUIRE_FALSE(rb.pop(dummy));
  // Out-value should not have been written to.
  REQUIRE(dummy == 0xAA);
}

TEST_CASE("RingBuffer: wrap-around correctness", "[ring_buffer]") {
  RingBuffer<uint8_t, 4> rb;
  // Fill with 1,2,3 — push 3 of 4 slots.
  REQUIRE(rb.push(1));
  REQUIRE(rb.push(2));
  REQUIRE(rb.push(3));
  // Pop 2 -> head=3, tail=2, size=1.
  uint8_t out = 0;
  REQUIRE(rb.pop(out));
  REQUIRE(out == 1);
  REQUIRE(rb.pop(out));
  REQUIRE(out == 2);
  // Push 3 more — this forces wrap since N=4.
  REQUIRE(rb.push(4));
  REQUIRE(rb.push(5));
  REQUIRE(rb.push(6));
  REQUIRE(rb.size() == 4);
  REQUIRE(rb.full());
  // Drain preserves order 3,4,5,6.
  REQUIRE(rb.pop(out)); REQUIRE(out == 3);
  REQUIRE(rb.pop(out)); REQUIRE(out == 4);
  REQUIRE(rb.pop(out)); REQUIRE(out == 5);
  REQUIRE(rb.pop(out)); REQUIRE(out == 6);
  REQUIRE(rb.empty());
}

TEST_CASE("RingBuffer: works with non-uint8 element type", "[ring_buffer]") {
  RingBuffer<int, 5> rb;
  for (int i = -2; i <= 2; ++i) {
    REQUIRE(rb.push(i));
  }
  REQUIRE(rb.full());
  int out = 0;
  int expected = -2;
  while (rb.pop(out)) {
    REQUIRE(out == expected);
    ++expected;
  }
  REQUIRE(expected == 3);
}

TEST_CASE("RingBuffer: size() tracks fills and drains", "[ring_buffer]") {
  RingBuffer<uint8_t, 8> rb;
  REQUIRE(rb.size() == 0);
  rb.push(1); rb.push(2); rb.push(3);
  REQUIRE(rb.size() == 3);
  REQUIRE(rb.free_space() == 5);
  uint8_t out = 0;
  rb.pop(out); rb.pop(out);
  REQUIRE(rb.size() == 1);
  REQUIRE(rb.free_space() == 7);
}

TEST_CASE("RingBuffer: peek does not consume", "[ring_buffer]") {
  RingBuffer<uint8_t, 4> rb;
  rb.push(99);
  uint8_t p = 0;
  REQUIRE(rb.peek(p));
  REQUIRE(p == 99);
  REQUIRE(rb.size() == 1);
  uint8_t o = 0;
  REQUIRE(rb.pop(o));
  REQUIRE(o == 99);
}

TEST_CASE("RingBuffer: push_overwrite drops oldest when full", "[ring_buffer]") {
  RingBuffer<uint8_t, 3> rb;
  rb.push(1); rb.push(2); rb.push(3);
  REQUIRE(rb.full());
  rb.push_overwrite(4);  // drops 1, pushes 4
  REQUIRE(rb.full());
  REQUIRE(rb.size() == 3);
  uint8_t out = 0;
  REQUIRE(rb.pop(out)); REQUIRE(out == 2);
  REQUIRE(rb.pop(out)); REQUIRE(out == 3);
  REQUIRE(rb.pop(out)); REQUIRE(out == 4);
}

TEST_CASE("RingBuffer: clear() resets state", "[ring_buffer]") {
  RingBuffer<uint8_t, 4> rb;
  rb.push(1); rb.push(2);
  rb.clear();
  REQUIRE(rb.empty());
  REQUIRE(rb.size() == 0);
  // Reuse after clear.
  REQUIRE(rb.push(10));
  uint8_t out = 0;
  REQUIRE(rb.pop(out));
  REQUIRE(out == 10);
}
