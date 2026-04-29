#include "capture.h"

// LittleFS / time-of-day pieces are Arduino-framework only. Host unit
// tests build with USE_ARDUINO undefined and exercise the pure
// serializers directly; the on-device persistence path is excluded.
#ifdef USE_ARDUINO
#include <Arduino.h>
// LittleFS.h transitively includes FS.h on Arduino-ESP32; declaring
// FS/LittleFS as build deps in __init__.py is what gets the headers
// onto the include path.
#include <LittleFS.h>
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

// ---------- Filesystem (Arduino only) ---------------------------------------

#ifdef USE_ARDUINO

bool Capture::ensure_root_directory_() {
  LittleFS.mkdir(root_path_.c_str());
  // Functional probe: arduino-esp32's `LittleFS.exists()` and
  // `isDirectory()` aren't always reliable on a freshly-formatted FS,
  // so test directly by writing a small marker and removing it. If
  // the write succeeds the directory definitely exists.
  std::string probe = root_path_ + "/.txcp-probe";
  File mf = LittleFS.open(probe.c_str(), "w");
  if (!mf) return false;
  mf.write(reinterpret_cast<const uint8_t *>("TXCP"), 4);
  mf.close();
  LittleFS.remove(probe.c_str());
  File dir = LittleFS.open(root_path_.c_str());
  bool ok = (bool) dir && dir.isDirectory();
  if (dir) dir.close();
  return ok;
}

void Capture::setup() {
  if (fs_ready_) return;
  if (!LittleFS.begin(true /* format on fail */)) {
    ESP_LOGE(TAG, "LittleFS mount failed; capture disabled");
    return;
  }
  if (!ensure_root_directory_()) {
    // Mount succeeded but the directory state is broken (seen on
    // arduino-esp32 LittleFS after a partial format-on-fail recovery
    // from a previously-foreign firmware partition). Force a full
    // format and remount, then retry once. We accept the data loss
    // because (a) the broken state was already losing every event the
    // capture writer attempted to flush and (b) this device has no
    // valuable LittleFS state outside captures.
    ESP_LOGW(TAG, "Capture: root directory not materialising — forcing "
                  "LittleFS.format() to clear corrupt state");
    LittleFS.format();
    LittleFS.begin(true);
    if (!ensure_root_directory_()) {
      ESP_LOGE(TAG, "Capture: filesystem still broken after format; "
                    "captures disabled");
      return;
    }
    ESP_LOGI(TAG, "Capture: filesystem recovered via format()");
  }
  fs_ready_ = true;
  ESP_LOGI(TAG, "Capture ready: root=%s mode=%s max=%u",
           root_path_.c_str(), mode_str(), (unsigned) max_file_bytes_);
}

void Capture::on_session_start() {
  session_active_ = true;
  session_start_us_ = (uint64_t) micros();
  if (mode_ == Mode::None) return;
  if (mode_ == Mode::MonitorOnly) return;  // capture only Monitor traffic
  open_new_file_();
}

void Capture::on_session_end() {
  // Flush queued events to disk before closing.
  flush_ring_();
  close_current_file_();
  session_active_ = false;
}

void Capture::open_new_file_() {
  if (!fs_ready_) {
    setup();
    if (!fs_ready_) return;
  }
  close_current_file_();

  uint32_t seq = ++file_seq_;
  time_t now = 0;
  ::time(&now);
  char path_bin[96];
  char path_txt[96];
  if (now > 1700000000) {
    snprintf(path_bin, sizeof(path_bin), "%s/wintex-%lu-%03u.bin",
             root_path_.c_str(), (unsigned long) now, (unsigned) seq);
    snprintf(path_txt, sizeof(path_txt), "%s/wintex-%lu-%03u.txt",
             root_path_.c_str(), (unsigned long) now, (unsigned) seq);
  } else {
    snprintf(path_bin, sizeof(path_bin), "%s/wintex-boot%lu-%03u.bin",
             root_path_.c_str(), (unsigned long) millis(), (unsigned) seq);
    snprintf(path_txt, sizeof(path_txt), "%s/wintex-boot%lu-%03u.txt",
             root_path_.c_str(), (unsigned long) millis(), (unsigned) seq);
  }
  current_path_bin_ = path_bin;
  current_path_txt_ = path_txt;

  // Write header to the .bin and a banner to the .txt.
  CaptureHeader h;
  h.version = kCaptureFormatVersion;
  h.start_unix_s = (now > 1700000000) ? (uint32_t) now : 0;
  h.set_panel_name(panel_name_);
  h.baud_rate = baud_rate_;
  uint8_t hdr[kCaptureHeaderSize];
  serialize_header(h, hdr);

  // Defensive mkdir on every session start — works around an edge case
  // where setup()'s mkdir silently fails on a freshly-formatted FS,
  // leaving subsequent file opens to fail. Idempotent if the dir
  // already exists.
  LittleFS.mkdir(root_path_.c_str());

  File f = LittleFS.open(current_path_bin_.c_str(), "w");
  if (!f) {
    // First open failed. Most likely cause: the parent directory wasn't
    // really created. Try mkdir + reopen one more time before giving up.
    LittleFS.mkdir(root_path_.c_str());
    f = LittleFS.open(current_path_bin_.c_str(), "w");
  }
  if (!f) {
    ESP_LOGW(TAG, "capture: failed to open %s — capture for this session will be lost",
             current_path_bin_.c_str());
    current_path_bin_.clear();
    current_path_txt_.clear();
    current_file_bytes_ = 0;
    return;
  }
  f.write(hdr, sizeof(hdr));
  f.close();

  File t = LittleFS.open(current_path_txt_.c_str(), "w");
  if (t) {
    char banner[128];
    int n = snprintf(banner, sizeof(banner),
                     "# TXCP capture v%u panel=%s baud=%u start_unix=%lu\n",
                     (unsigned) kCaptureFormatVersion, panel_name_.c_str(),
                     (unsigned) baud_rate_, (unsigned long) h.start_unix_s);
    if (n > 0) t.write(reinterpret_cast<const uint8_t *>(banner), n);
    t.close();
  } else {
    ESP_LOGW(TAG, "capture: failed to open sidecar %s",
             current_path_txt_.c_str());
  }

  current_file_bytes_ = sizeof(hdr);
  ESP_LOGI(TAG, "capture: opened %s", current_path_bin_.c_str());
}

void Capture::close_current_file_() {
  if (current_path_bin_.empty()) return;
  ESP_LOGI(TAG, "capture: closed %s (%u bytes)", current_path_bin_.c_str(),
           (unsigned) current_file_bytes_);
  current_path_bin_.clear();
  current_path_txt_.clear();
  current_file_bytes_ = 0;
}

void Capture::write_bytes_(const uint8_t *bytes, std::size_t len) {
  if (test_sink_) {
    test_sink_(bytes, len);
    return;
  }
  if (!fs_ready_ || current_path_bin_.empty()) {
    // No file open — drop. record() shouldn't get here in BridgeOnly
    // mode, but guard anyway.
    ++drops_;
    return;
  }
  // Rotate before writing so the new event lands in a fresh file.
  if (current_file_bytes_ + len > max_file_bytes_) {
    open_new_file_();
  }
  File f = LittleFS.open(current_path_bin_.c_str(), "a");
  if (!f) {
    ++drops_;
    return;
  }
  std::size_t written = f.write(bytes, len);
  f.close();
  if (written != len) {
    ++drops_;
    return;
  }
  // Append the same bytes to the hex sidecar.
  write_hexdump_(current_file_bytes_, bytes, len);
  current_file_bytes_ += len;
  bytes_written_total_ += len;
}

void Capture::write_hexdump_(uint64_t file_offset, const uint8_t *bytes,
                             std::size_t len) {
  File t = LittleFS.open(current_path_txt_.c_str(), "a");
  if (!t) return;
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
    t.write(reinterpret_cast<const uint8_t *>(line), pos);
  }
  t.close();
}

#else  // !USE_ARDUINO — host test build

void Capture::setup() { fs_ready_ = true; }

void Capture::on_session_start() {
  session_active_ = true;
  session_start_us_ = 0;
  if (mode_ == Mode::None || mode_ == Mode::MonitorOnly) return;
  // In host tests, "opening a file" means writing the header to the
  // test sink so callers can assert on the byte stream.
  if (test_sink_) {
    CaptureHeader h;
    h.version = kCaptureFormatVersion;
    h.start_unix_s = 0;
    h.set_panel_name(panel_name_);
    h.baud_rate = baud_rate_;
    uint8_t hdr[kCaptureHeaderSize];
    serialize_header(h, hdr);
    test_sink_(hdr, sizeof(hdr));
    current_file_bytes_ = sizeof(hdr);
  }
}

void Capture::on_session_end() {
  flush_ring_();
  session_active_ = false;
  current_file_bytes_ = 0;
}

void Capture::write_bytes_(const uint8_t *bytes, std::size_t len) {
  if (test_sink_) {
    test_sink_(bytes, len);
    current_file_bytes_ += len;
    bytes_written_total_ += len;
  }
}

void Capture::open_new_file_() {}
void Capture::close_current_file_() {}
void Capture::write_hexdump_(uint64_t, const uint8_t *, std::size_t) {}

#endif  // USE_ARDUINO

}  // namespace texecom
}  // namespace esphome
