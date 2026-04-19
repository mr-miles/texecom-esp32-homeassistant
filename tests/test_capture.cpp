// Host-side unit tests for the TXCP capture binary format (Plan 02-01).
//
// These exercise the pure serializers/parsers in capture.h/.cpp. The
// LittleFS-touching code is gated behind USE_ARDUINO and is NOT compiled
// here — host tests only validate the on-disk byte layout that Plan
// 02-02's decoder will consume.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "capture.h"

using esphome::texecom::Capture;
using esphome::texecom::CaptureHeader;
using esphome::texecom::kCaptureEventFixedSize;
using esphome::texecom::kCaptureFormatVersion;
using esphome::texecom::kCaptureHeaderSize;
using esphome::texecom::kCaptureMagic;
using esphome::texecom::kCapturePanelNameLen;
using esphome::texecom::parse_event;
using esphome::texecom::parse_header;
using esphome::texecom::serialize_event;
using esphome::texecom::serialize_header;

TEST_CASE("Capture: header round-trip preserves all fields", "[capture][header]") {
  CaptureHeader in;
  in.version = kCaptureFormatVersion;
  in.start_unix_s = 1717000000u;
  in.set_panel_name("Premier 24");
  in.baud_rate = 19200;

  uint8_t buf[kCaptureHeaderSize];
  std::size_t n = serialize_header(in, buf);
  REQUIRE(n == kCaptureHeaderSize);
  REQUIRE(n == 32);  // explicit guard against drift

  CaptureHeader out;
  REQUIRE(parse_header(buf, out));
  REQUIRE(out.version == kCaptureFormatVersion);
  REQUIRE(out.start_unix_s == 1717000000u);
  REQUIRE(out.baud_rate == 19200u);
  REQUIRE(std::strncmp(out.panel_name, "Premier 24", kCapturePanelNameLen) == 0);
  // Bytes after the name string must be zero-padded.
  REQUIRE(out.panel_name[10] == '\0');
  REQUIRE(out.panel_name[15] == '\0');
}

TEST_CASE("Capture: magic bytes are exactly 'T','X','C','P'", "[capture][magic]") {
  CaptureHeader in;
  in.set_panel_name("Premier 24");
  uint8_t buf[kCaptureHeaderSize];
  serialize_header(in, buf);
  REQUIRE(buf[0] == 'T');
  REQUIRE(buf[1] == 'X');
  REQUIRE(buf[2] == 'C');
  REQUIRE(buf[3] == 'P');
  // Constants table must agree.
  REQUIRE(kCaptureMagic[0] == 'T');
  REQUIRE(kCaptureMagic[1] == 'X');
  REQUIRE(kCaptureMagic[2] == 'C');
  REQUIRE(kCaptureMagic[3] == 'P');

  // Tampered magic must fail to parse.
  uint8_t bad[kCaptureHeaderSize];
  std::memcpy(bad, buf, sizeof(bad));
  bad[0] = 'X';
  CaptureHeader out;
  REQUIRE_FALSE(parse_header(bad, out));
}

TEST_CASE("Capture: single event round-trip", "[capture][event]") {
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  uint8_t buf[kCaptureEventFixedSize + sizeof(payload)];
  std::size_t n = serialize_event(/*direction=*/0, /*ts_us=*/12345ull, payload,
                                  sizeof(payload), buf);
  REQUIRE(n == kCaptureEventFixedSize + sizeof(payload));

  uint8_t direction = 99;
  uint64_t ts_us = 0;
  uint16_t length = 0;
  const uint8_t *recovered = nullptr;
  std::size_t consumed = parse_event(buf, n, direction, ts_us, length, recovered);
  REQUIRE(consumed == n);
  REQUIRE(direction == 0);
  REQUIRE(ts_us == 12345ull);
  REQUIRE(length == sizeof(payload));
  REQUIRE(recovered != nullptr);
  REQUIRE(recovered[0] == 0x01);
  REQUIRE(recovered[1] == 0x02);
  REQUIRE(recovered[2] == 0x03);
}

TEST_CASE("Capture: multiple events back-to-back parse linearly",
          "[capture][event][stream]") {
  std::vector<uint8_t> stream;
  struct Sample {
    uint8_t dir;
    uint64_t ts;
    std::vector<uint8_t> bytes;
  };
  const Sample samples[] = {
      {0, 100, {0xAA}},
      {1, 250, {0xBB, 0xCC}},
      {0, 999, {0xDE, 0xAD, 0xBE, 0xEF}},
      {1, 1500, {}},  // zero-length is legal
      {0, 1700, {0x42, 0x42, 0x42, 0x42, 0x42}},
  };

  for (const auto &s : samples) {
    uint8_t scratch[kCaptureEventFixedSize + 32];
    std::size_t n = serialize_event(s.dir, s.ts, s.bytes.data(), s.bytes.size(),
                                    scratch);
    REQUIRE(n == kCaptureEventFixedSize + s.bytes.size());
    stream.insert(stream.end(), scratch, scratch + n);
  }

  std::size_t off = 0;
  for (const auto &s : samples) {
    uint8_t direction = 99;
    uint64_t ts = 0;
    uint16_t len = 0;
    const uint8_t *payload = nullptr;
    std::size_t consumed = parse_event(stream.data() + off, stream.size() - off,
                                       direction, ts, len, payload);
    REQUIRE(consumed > 0);
    REQUIRE(direction == s.dir);
    REQUIRE(ts == s.ts);
    REQUIRE(len == s.bytes.size());
    for (std::size_t i = 0; i < s.bytes.size(); ++i) {
      REQUIRE(payload[i] == s.bytes[i]);
    }
    off += consumed;
  }
  REQUIRE(off == stream.size());
}

TEST_CASE("Capture: length field round-trips at boundary values",
          "[capture][event][length]") {
  // Test 0 / 1 / 256 / 1023 — covers single-byte, byte boundary, and
  // a value that exercises both bytes of the LE length field.
  for (std::size_t len : {std::size_t{0}, std::size_t{1}, std::size_t{256},
                           std::size_t{1023}}) {
    std::vector<uint8_t> payload(len);
    for (std::size_t i = 0; i < len; ++i) {
      payload[i] = static_cast<uint8_t>(i & 0xFF);
    }
    std::vector<uint8_t> buf(kCaptureEventFixedSize + len);
    std::size_t n = serialize_event(1, 42ull, payload.data(), len, buf.data());
    REQUIRE(n == kCaptureEventFixedSize + len);

    // Confirm length bytes are little-endian.
    uint16_t encoded_len =
        static_cast<uint16_t>(buf[9]) | (static_cast<uint16_t>(buf[10]) << 8);
    REQUIRE(encoded_len == len);

    uint8_t dir = 99;
    uint64_t ts = 0;
    uint16_t parsed_len = 0;
    const uint8_t *recovered = nullptr;
    std::size_t consumed =
        parse_event(buf.data(), buf.size(), dir, ts, parsed_len, recovered);
    REQUIRE(consumed == n);
    REQUIRE(dir == 1);
    REQUIRE(ts == 42ull);
    REQUIRE(parsed_len == len);
    for (std::size_t i = 0; i < len; ++i) {
      REQUIRE(recovered[i] == payload[i]);
    }
  }
}

TEST_CASE("Capture: invalid direction values are rejected",
          "[capture][event][validation]") {
  // Per the locked TXCP spec, direction must be 0 or 1. Out-of-range
  // values cause serialize_event() to return 0 (event NOT written) so
  // the caller can detect the misuse without corrupting the stream.
  uint8_t buf[64];
  REQUIRE(serialize_event(0, 1, nullptr, 0, buf) == kCaptureEventFixedSize);
  REQUIRE(serialize_event(1, 1, nullptr, 0, buf) == kCaptureEventFixedSize);
  REQUIRE(serialize_event(2, 1, nullptr, 0, buf) == 0);
  REQUIRE(serialize_event(0xFF, 1, nullptr, 0, buf) == 0);

  // parse_event() symmetrically refuses a stream with a bogus direction.
  uint8_t bad[kCaptureEventFixedSize] = {0};
  bad[0] = 0x05;  // invalid direction
  uint8_t dir = 0;
  uint64_t ts = 0;
  uint16_t len = 0;
  const uint8_t *p = nullptr;
  REQUIRE(parse_event(bad, sizeof(bad), dir, ts, len, p) == 0);
}

TEST_CASE("Capture: parse_event refuses truncated input",
          "[capture][event][validation]") {
  const uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD};
  uint8_t buf[kCaptureEventFixedSize + sizeof(payload)];
  std::size_t n = serialize_event(0, 7ull, payload, sizeof(payload), buf);
  REQUIRE(n == sizeof(buf));

  // Truncate at every offset before the full record and confirm
  // parse_event() cleanly reports failure rather than reading past end.
  for (std::size_t trunc = 0; trunc < n; ++trunc) {
    uint8_t dir = 0;
    uint64_t ts = 0;
    uint16_t len = 0;
    const uint8_t *p = nullptr;
    REQUIRE(parse_event(buf, trunc, dir, ts, len, p) == 0);
  }
}

TEST_CASE("Capture: end-to-end via test sink replays one session",
          "[capture][integration]") {
  // Drive Capture through a minimal session and verify the byte stream
  // it would have written to LittleFS parses back to the same events.
  std::vector<uint8_t> sink;
  Capture cap;
  cap.set_panel_name("Premier 24");
  cap.set_mode(Capture::Mode::BridgeOnly);
  cap.set_sink_for_test(
      [&](const uint8_t *p, std::size_t n) { sink.insert(sink.end(), p, p + n); });
  cap.setup();
  cap.on_session_start();  // emits header

  const uint8_t panel_to_client[] = {0x10, 0x20, 0x30};
  const uint8_t client_to_panel[] = {0xA1, 0xA2};
  REQUIRE(cap.record(0, panel_to_client, sizeof(panel_to_client)));
  REQUIRE(cap.record(1, client_to_panel, sizeof(client_to_panel)));
  cap.on_session_end();  // flushes ring

  REQUIRE(sink.size() >= kCaptureHeaderSize);
  CaptureHeader hdr;
  REQUIRE(parse_header(sink.data(), hdr));
  REQUIRE(std::strncmp(hdr.panel_name, "Premier 24", kCapturePanelNameLen) == 0);

  std::size_t off = kCaptureHeaderSize;
  uint8_t dir = 9;
  uint64_t ts = 0;
  uint16_t len = 0;
  const uint8_t *payload = nullptr;
  std::size_t c = parse_event(sink.data() + off, sink.size() - off, dir, ts, len, payload);
  REQUIRE(c > 0);
  REQUIRE(dir == 0);
  REQUIRE(len == sizeof(panel_to_client));
  REQUIRE(payload[0] == 0x10);
  off += c;

  c = parse_event(sink.data() + off, sink.size() - off, dir, ts, len, payload);
  REQUIRE(c > 0);
  REQUIRE(dir == 1);
  REQUIRE(len == sizeof(client_to_panel));
  REQUIRE(payload[0] == 0xA1);
  off += c;

  REQUIRE(off == sink.size());
}
