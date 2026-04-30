#include "capture.h"

// On-device millis()/time() are Arduino-framework only. Host unit tests
// build with USE_ARDUINO undefined and exercise the pure serializers
// directly; the on-device persistence path is excluded.
#ifdef USE_ARDUINO
#include <Arduino.h>
#include <time.h>
#include "esphome/core/log.h"
#endif

#include <cstdio>

namespace esphome {
namespace texecom {

#ifdef USE_ARDUINO
static const char *const TAG = "texecom.capture";
#endif

// ---------- Pure serializers (host-testable) -------------------------------

namespace {

inline void write_u16_le(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void write_u32_le(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline void write_u64_le(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

inline uint16_t read_u16_le(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32_le(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_u64_le(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= (static_cast<uint64_t>(p[i]) << (8 * i));
  }
  return v;
}

}  // namespace

std::size_t serialize_header(const CaptureHeader &h, uint8_t *out) {
  // Magic: "TXCP" — LOCKED, do not change.
  out[0] = kCaptureMagic[0];
  out[1] = kCaptureMagic[1];
  out[2] = kCaptureMagic[2];
  out[3] = kCaptureMagic[3];
  write_u16_le(out + 4, h.version);
  write_u32_le(out + 6, h.start_unix_s);
  std::memcpy(out + 10, h.panel_name, kCapturePanelNameLen);
  write_u32_le(out + 26, h.baud_rate);
  out[30] = 0;
  out[31] = 0;
  return kCaptureHeaderSize;
}

bool parse_header(const uint8_t *in, CaptureHeader &h) {
  if (in[0] != kCaptureMagic[0] || in[1] != kCaptureMagic[1] ||
      in[2] != kCaptureMagic[2] || in[3] != kCaptureMagic[3]) {
    return false;
  }
  h.version = read_u16_le(in + 4);
  if (h.version != kCaptureFormatVersion) {
    // Unknown version: caller can decide. Fields below may be garbage.
    return false;
  }
  h.start_unix_s = read_u32_le(in + 6);
  std::memcpy(h.panel_name, in + 10, kCapturePanelNameLen);
  h.baud_rate = read_u32_le(in + 26);
  return true;
}

std::size_t serialize_event(uint8_t direction, uint64_t timestamp_us,
                            const uint8_t *bytes, std::size_t len, uint8_t *out) {
  // Direction is restricted to {0, 1} by the format spec.
  if (direction > 1) {
    return 0;
  }
  if (len > 0xFFFF) {
    return 0;
  }
  out[0] = direction;
  write_u64_le(out + 1, timestamp_us);
  write_u16_le(out + 9, static_cast<uint16_t>(len));
  if (len > 0 && bytes != nullptr) {
    std::memcpy(out + kCaptureEventFixedSize, bytes, len);
  }
  return kCaptureEventFixedSize + len;
}

std::size_t parse_event(const uint8_t *in, std::size_t in_len,
                        uint8_t &direction, uint64_t &timestamp_us,
                        uint16_t &length, const uint8_t *&payload) {
  if (in_len < kCaptureEventFixedSize) {
    return 0;
  }
  direction = in[0];
  if (direction > 1) {
    return 0;
  }
  timestamp_us = read_u64_le(in + 1);
  length = read_u16_le(in + 9);
  if (in_len < kCaptureEventFixedSize + length) {
    return 0;
  }
  payload = in + kCaptureEventFixedSize;
  return kCaptureEventFixedSize + length;
}

// ---------- Diagnostic strings ---------------------------------------------

const char *Capture::mode_str() const {
  switch (mode_) {
    case Mode::None: return "none";
    case Mode::BridgeOnly: return "bridge";
    case Mode::MonitorOnly: return "monitor";
    case Mode::Both: return "both";
  }
  return "?";
}

// ---------- Snapshot accessors (always built) ------------------------------

std::vector<RamCaptureSnapshot> Capture::list_captures() const {
  std::vector<RamCaptureSnapshot> out;
  out.reserve(captures_.size());
  for (const auto &c : captures_) {
    out.push_back({c.name, c.bytes.size(), c.mtime_ms});
  }
  return out;
}

const std::vector<uint8_t> *Capture::get_capture_bytes(const std::string &name) const {
  for (const auto &c : captures_) {
    if (c.name == name) return &c.bytes;
  }
  return nullptr;
}

// ---------- Ring helpers ----------------------------------------------------

void Capture::enqueue_event_(uint8_t direction, const uint8_t *bytes, std::size_t len) {
  // Split oversize pushes into multiple events so the in-ring payload
  // bound (kMaxEventPayload) doesn't lose data. Wintex frames stay well
  // below 256B in practice; this is purely defensive.
  std::size_t off = 0;
  while (off < len) {
    if (ring_size_ == ring_.size()) {
      ++drops_;
#ifdef USE_ARDUINO
      if ((warn_throttle_++ & 0x3F) == 0) {
        ESP_LOGW(TAG, "capture ring full — dropping events (total drops=%u)",
                 (unsigned) drops_);
      }
#endif
      return;
    }
    std::size_t chunk = len - off;
    if (chunk > kMaxEventPayload) chunk = kMaxEventPayload;

    PendingEvent &e = ring_[ring_head_];
    e.direction = direction;
#ifdef USE_ARDUINO
    e.ts_us = (uint64_t) micros() - session_start_us_;
#else
    e.ts_us = 0;  // host tests set ts via direct serialize_event() calls
#endif
    e.len = static_cast<uint16_t>(chunk);
    if (bytes != nullptr) {
      std::memcpy(e.bytes, bytes + off, chunk);
    }
    ring_head_ = (ring_head_ + 1) % ring_.size();
    ++ring_size_;
    off += chunk;
  }
}

void Capture::flush_ring_() {
  // Drain everything currently queued. Each pop becomes one
  // serialize_event() write.
  uint8_t scratch[kCaptureEventFixedSize + kMaxEventPayload];
  while (ring_size_ > 0) {
    PendingEvent &e = ring_[ring_tail_];
    std::size_t n = serialize_event(e.direction, e.ts_us, e.bytes, e.len, scratch);
    if (n > 0) {
      write_bytes_(scratch, n);
    }
    ring_tail_ = (ring_tail_ + 1) % ring_.size();
    --ring_size_;
  }
}

// ---------- Public lifecycle ------------------------------------------------

bool Capture::record(uint8_t direction, const uint8_t *bytes, std::size_t len) {
  if (mode_ == Mode::None) return false;
  if (direction > 1) return false;
  if (len == 0) return false;
  if (mode_ == Mode::BridgeOnly && !session_active_) return false;
  // MonitorOnly: skip while a Bridge session is active.
  if (mode_ == Mode::MonitorOnly && session_active_) return false;

  enqueue_event_(direction, bytes, len);
  return true;
}

void Capture::loop() {
  // Flush opportunistically whenever the loop ticks. Bounded work — we
  // process whatever's queued and return. A 64-event burst at average
  // ~16B per event = ~1KB of write per tick worst case.
  if (ring_size_ == 0) return;
  flush_ring_();
}

// ---------- In-RAM storage helpers (host + Arduino) ------------------------

void Capture::append_to_(int idx, const uint8_t *bytes, std::size_t len) {
  if (idx < 0 || idx >= static_cast<int>(captures_.size())) return;
  if (len == 0) return;
  RamCapture &c = captures_[idx];
  c.bytes.insert(c.bytes.end(), bytes, bytes + len);
  total_bytes_used_ += len;
#ifdef USE_ARDUINO
  c.mtime_ms = (uint32_t) millis();
#else
  c.mtime_ms = 0;
#endif
}

void Capture::enforce_budget_() {
  // Evict oldest first. Skip the live .bin / .txt — truncating mid-
  // session would corrupt active events. If both live captures alone
  // exceed the budget, log once and bail (overflow is preferable to
  // breaking the live stream).
  while (total_bytes_used_ > max_total_bytes_) {
    int victim = -1;
    for (std::size_t i = 0; i < captures_.size(); ++i) {
      int signed_i = static_cast<int>(i);
      if (signed_i == current_bin_idx_) continue;
      if (signed_i == current_txt_idx_) continue;
      victim = signed_i;
      break;
    }
    if (victim < 0) {
      if (!budget_warn_emitted_) {
#ifdef USE_ARDUINO
        ESP_LOGW(TAG,
                 "capture: live session exceeds RAM budget (%u > %u); "
                 "letting it overflow until session ends",
                 (unsigned) total_bytes_used_, (unsigned) max_total_bytes_);
#endif
        budget_warn_emitted_ = true;
      }
      return;
    }
    total_bytes_used_ -= captures_[victim].bytes.size();
    captures_.erase(captures_.begin() + victim);
    // Adjust live indices: erasing at index `victim` shifts every
    // subsequent index down by one.
    if (current_bin_idx_ > victim) --current_bin_idx_;
    if (current_txt_idx_ > victim) --current_txt_idx_;
  }
}

void Capture::open_new_capture_() {
  close_current_capture_();

  uint32_t seq = ++file_seq_;
  uint32_t now_unix = 0;
  uint32_t now_ms = 0;
#ifdef USE_ARDUINO
  time_t now = 0;
  ::time(&now);
  if (now > 1700000000) now_unix = (uint32_t) now;
  now_ms = (uint32_t) millis();
#endif

  char name_bin[64];
  char name_txt[64];
  if (now_unix > 0) {
    snprintf(name_bin, sizeof(name_bin), "wintex-%lu-%03u.bin",
             (unsigned long) now_unix, (unsigned) seq);
    snprintf(name_txt, sizeof(name_txt), "wintex-%lu-%03u.txt",
             (unsigned long) now_unix, (unsigned) seq);
  } else {
    snprintf(name_bin, sizeof(name_bin), "wintex-boot%lu-%03u.bin",
             (unsigned long) now_ms, (unsigned) seq);
    snprintf(name_txt, sizeof(name_txt), "wintex-boot%lu-%03u.txt",
             (unsigned long) now_ms, (unsigned) seq);
  }

  // Build header bytes for the .bin.
  CaptureHeader h;
  h.version = kCaptureFormatVersion;
  h.start_unix_s = now_unix;
  h.set_panel_name(panel_name_);
  h.baud_rate = baud_rate_;
  uint8_t hdr[kCaptureHeaderSize];
  serialize_header(h, hdr);

  // Build text-banner for the .txt.
  char banner[128];
  int banner_n = snprintf(banner, sizeof(banner),
                          "# TXCP capture v%u panel=%s baud=%u start_unix=%lu\n",
                          (unsigned) kCaptureFormatVersion, panel_name_.c_str(),
                          (unsigned) baud_rate_, (unsigned long) h.start_unix_s);
  if (banner_n < 0) banner_n = 0;

  // Push the .bin entry first, then the .txt. Indices reflect insertion
  // order; eviction will adjust them if either is removed.
  RamCapture bin;
  bin.name = name_bin;
  bin.is_text = false;
  bin.mtime_ms = now_ms;
  bin.bytes.reserve(kCaptureHeaderSize + 4096);
  bin.bytes.insert(bin.bytes.end(), hdr, hdr + kCaptureHeaderSize);
  captures_.push_back(std::move(bin));
  current_bin_idx_ = static_cast<int>(captures_.size()) - 1;
  total_bytes_used_ += kCaptureHeaderSize;

  RamCapture txt;
  txt.name = name_txt;
  txt.is_text = true;
  txt.mtime_ms = now_ms;
  txt.bytes.reserve(static_cast<std::size_t>(banner_n) + 4096);
  if (banner_n > 0) {
    txt.bytes.insert(txt.bytes.end(),
                     reinterpret_cast<const uint8_t *>(banner),
                     reinterpret_cast<const uint8_t *>(banner) + banner_n);
  }
  captures_.push_back(std::move(txt));
  current_txt_idx_ = static_cast<int>(captures_.size()) - 1;
  total_bytes_used_ += static_cast<std::size_t>(banner_n);

#ifdef USE_ARDUINO
  ESP_LOGI(TAG, "capture: opened in-RAM file %s (+%s)",
           captures_[current_bin_idx_].name.c_str(),
           captures_[current_txt_idx_].name.c_str());
#endif

  // Apply budget after opening — if the new pair pushes us over budget,
  // older captures are evicted first (current pair is protected).
  enforce_budget_();
  budget_warn_emitted_ = false;
}

void Capture::close_current_capture_() {
  if (current_bin_idx_ < 0) return;
#ifdef USE_ARDUINO
  std::size_t bin_size = captures_[current_bin_idx_].bytes.size();
  ESP_LOGI(TAG, "capture: closed %s (%u bytes total in RAM)",
           captures_[current_bin_idx_].name.c_str(),
           (unsigned) total_bytes_used_);
  (void) bin_size;
#endif
  current_bin_idx_ = -1;
  current_txt_idx_ = -1;
}

void Capture::write_bytes_(const uint8_t *bytes, std::size_t len) {
  if (test_sink_) {
    test_sink_(bytes, len);
    bytes_written_total_ += len;
    return;
  }
  if (current_bin_idx_ < 0) {
    // No live capture — drop. record() shouldn't get here in BridgeOnly
    // mode, but guard anyway.
    ++drops_;
    return;
  }
  // The hex sidecar uses the .bin's pre-write offset so the displayed
  // address column matches the .bin's byte stream. Capture that BEFORE
  // appending to the .bin.
  uint64_t bin_offset = captures_[current_bin_idx_].bytes.size();
  append_to_(current_bin_idx_, bytes, len);
  bytes_written_total_ += len;
  write_hexdump_(bin_offset, bytes, len);
  enforce_budget_();
}

void Capture::write_hexdump_(uint64_t file_offset, const uint8_t *bytes,
                             std::size_t len) {
  if (current_txt_idx_ < 0) return;
  // 16 bytes per line, "00000010  00 11 22 ...  |ASCII|"
  char line[96];
  for (std::size_t i = 0; i < len; i += 16) {
    std::size_t row = (len - i < 16) ? (len - i) : 16;
    int pos = snprintf(line, sizeof(line), "%08lx  ",
                       (unsigned long) (file_offset + i));
    for (std::size_t j = 0; j < 16; ++j) {
      if (j < row) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02x ", bytes[i + j]);
      } else {
        pos += snprintf(line + pos, sizeof(line) - pos, "   ");
      }
    }
    pos += snprintf(line + pos, sizeof(line) - pos, " |");
    for (std::size_t j = 0; j < row && pos < (int) sizeof(line) - 4; ++j) {
      uint8_t c = bytes[i + j];
      line[pos++] = (c >= 0x20 && c < 0x7F) ? (char) c : '.';
    }
    if (pos < (int) sizeof(line) - 2) {
      line[pos++] = '|';
      line[pos++] = '\n';
    }
    append_to_(current_txt_idx_, reinterpret_cast<const uint8_t *>(line),
               static_cast<std::size_t>(pos));
  }
}

#ifdef USE_ARDUINO

void Capture::setup() {
  ESP_LOGI(TAG, "Capture ready: in-RAM (max=%u bytes, mode=%s)",
           (unsigned) max_total_bytes_, mode_str());
}

void Capture::on_session_start() {
  session_active_ = true;
  session_start_us_ = (uint64_t) micros();
  if (mode_ == Mode::None) return;
  if (mode_ == Mode::MonitorOnly) return;  // capture only Monitor traffic
  open_new_capture_();
}

void Capture::on_session_end() {
  // Flush queued events before marking the capture closed.
  flush_ring_();
  close_current_capture_();
  session_active_ = false;
}

#else  // !USE_ARDUINO — host test build

void Capture::setup() {}

void Capture::on_session_start() {
  session_active_ = true;
  session_start_us_ = 0;
  if (mode_ == Mode::None || mode_ == Mode::MonitorOnly) return;
  // In host tests with a sink set, "opening a file" means writing the
  // header to the sink so callers can assert on the byte stream.
  // Otherwise (no sink) we still populate the in-RAM store so the host
  // tests can exercise list_captures()/get_capture_bytes().
  if (test_sink_) {
    CaptureHeader h;
    h.version = kCaptureFormatVersion;
    h.start_unix_s = 0;
    h.set_panel_name(panel_name_);
    h.baud_rate = baud_rate_;
    uint8_t hdr[kCaptureHeaderSize];
    serialize_header(h, hdr);
    test_sink_(hdr, sizeof(hdr));
    bytes_written_total_ += sizeof(hdr);
  } else {
    open_new_capture_();
  }
}

void Capture::on_session_end() {
  flush_ring_();
  if (!test_sink_) {
    close_current_capture_();
  }
  session_active_ = false;
}

#endif  // USE_ARDUINO

}  // namespace texecom
}  // namespace esphome
