#pragma once

// On-device capture of bytes flowing through the Texecom bridge.
//
// Plan 02-01 deliverable. Records every byte the bridge moves in either
// direction with a relative microsecond timestamp into a rotating LittleFS
// file. Files are downloadable over HTTP via the ESPHome web_server; the
// binary format is documented below and host-side tested via
// `tests/test_capture.cpp` so the parser used in Plan 02-02 has a
// guaranteed wire-format contract.
//
// =====================================================================
// TXCP binary format v1 (LOCKED — do not change without bumping version)
// =====================================================================
//
// File layout: <header><event><event>...<event>
//
// File header — exactly 32 bytes, written once per file:
//   offset  size  field           notes
//   0       4     magic           ASCII "TXCP" (0x54 0x58 0x43 0x50)
//   4       2     version         uint16 little-endian, == 1
//   6       4     start_unix_s    uint32 LE, wall-clock seconds at file
//                                 open time, or 0 if SNTP not synced
//   10      16    panel_name      ASCII, null-padded, e.g. "Premier 24"
//   26      4     baud_rate       uint32 LE, e.g. 19200
//   30      2     reserved        zero
//
// Event record — variable length:
//   offset  size  field           notes
//   0       1     direction       0 = panel -> client (UART RX -> TCP)
//                                 1 = client -> panel (TCP -> UART TX)
//   1       8     timestamp_us    uint64 LE, microseconds since file open
//                                 (NOT wall-clock — boot-relative is
//                                 stable; wall-clock may not be available
//                                 on first boot before SNTP)
//   9       2     length          uint16 LE, count of payload bytes that
//                                 follow (0..65535)
//   11      N     bytes           payload (length bytes)
//
// Rationale:
//   * Fixed 32-byte header keeps offsets predictable for hex inspection.
//   * Boot-relative microsecond timestamps are precise and work even
//     before SNTP. The header's start_unix_s lets the host re-derive
//     wall-clock if SNTP synced.
//   * 2-byte length covers any plausible Wintex frame (largest observed
//     ~512B). Records can therefore be parsed linearly without any
//     framing markers, which keeps the parser trivial.
//
// Rotation:
//   * New file when the current file reaches `max_file_bytes` (default
//     262144 = 256 KB).
//   * New file on `on_session_start()` (Bridge mode begins) and
//     `on_session_end()` (Bridge mode ends) so each Wintex session lands
//     in its own pair of files.
//   * Filename: "{root}/wintex-{unix_ts}-{seq}.bin" if SNTP synced, else
//     "{root}/wintex-boot{boot_ms}-{seq}.bin". Sequence counter avoids
//     collisions inside one boot.
//
// Hex-dump sidecar:
//   * For each .bin a paired .txt is written incrementally (xxd-style:
//     16 bytes/line, hex offset, hex bytes, ASCII gutter). Browsing the
//     .txt in a browser gives a human-readable view without downloading.
//
// Backpressure:
//   * In-memory ring buffer of N events (default 64). If full and the
//     LittleFS flush is slow, the new event is dropped and `drops_` is
//     incremented; a single WARN log fires per overflow burst. The bridge
//     NEVER blocks on capture — losing capture bytes is preferable to
//     stalling the UART pipe.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace esphome {
namespace texecom {

// Locked format constants — do not edit.
static constexpr std::size_t kCaptureHeaderSize = 32;
static constexpr std::size_t kCaptureEventFixedSize = 11;  // direction + ts + len
static constexpr uint16_t kCaptureFormatVersion = 1;
static constexpr uint8_t kCaptureMagic[4] = {'T', 'X', 'C', 'P'};
static constexpr std::size_t kCapturePanelNameLen = 16;
static constexpr std::size_t kDefaultCaptureMaxFileBytes = 262144;  // 256 KB
static constexpr std::size_t kCaptureRingEvents = 64;

// Plain-old-data view of a header for serialize/parse. Kept POD so it
// can be filled in tests without depending on Capture's lifecycle.
struct CaptureHeader {
  uint16_t version{kCaptureFormatVersion};
  uint32_t start_unix_s{0};
  char panel_name[kCapturePanelNameLen]{};  // null-padded
  uint32_t baud_rate{19200};

  void set_panel_name(const std::string &n) {
    std::memset(panel_name, 0, sizeof(panel_name));
    std::size_t copy = n.size() < sizeof(panel_name) ? n.size() : sizeof(panel_name);
    std::memcpy(panel_name, n.data(), copy);
  }
};

// Pure serializer — writes exactly kCaptureHeaderSize bytes into `out`.
// Returns the number of bytes written (always kCaptureHeaderSize).
// Caller must guarantee `out` has at least kCaptureHeaderSize bytes.
std::size_t serialize_header(const CaptureHeader &h, uint8_t *out);

// Pure parser — reads kCaptureHeaderSize bytes from `in`, fills `h`.
// Returns true if the magic + version are valid; false otherwise.
bool parse_header(const uint8_t *in, CaptureHeader &h);

// Pure event serializer. Writes (kCaptureEventFixedSize + len) bytes into
// `out`. Caller must size `out` appropriately. Returns total bytes
// written. `direction` MUST be 0 or 1; values outside that range are
// caller error and yield 0 (event not written).
std::size_t serialize_event(uint8_t direction, uint64_t timestamp_us,
                            const uint8_t *bytes, std::size_t len, uint8_t *out);

// Pure event parser. Reads from `in` (with `in_len` bytes available).
// On success returns total bytes consumed; on failure returns 0 (e.g.
// truncated input). On success: direction/ts/len are populated and
// `*payload` points into `in` (no copy).
std::size_t parse_event(const uint8_t *in, std::size_t in_len,
                        uint8_t &direction, uint64_t &timestamp_us,
                        uint16_t &length, const uint8_t *&payload);

// Capture sink: owns the per-event ring buffer and (on-device) the
// LittleFS file handles.
class Capture {
 public:
  enum class Mode : uint8_t {
    None = 0,         // Disabled
    BridgeOnly = 1,   // Record only when a Wintex client is connected
    MonitorOnly = 2,  // Record only Monitor-mode panel chatter
    Both = 3,         // Always record
  };

  Capture() = default;

  // ---- Configuration (call before setup()) -----------------------------
  void set_mode(Mode m) { mode_ = m; }
  void set_panel_name(const std::string &name) { panel_name_ = name; }
  void set_max_file_bytes(std::size_t bytes) {
    max_file_bytes_ = bytes ? bytes : kDefaultCaptureMaxFileBytes;
  }
  void set_root_path(const std::string &path) { root_path_ = path; }
  void set_baud_rate(uint32_t baud) { baud_rate_ = baud; }

  // Test/host hook: when set, serialized bytes are sent to this sink
  // INSTEAD OF being written to LittleFS. Lets host tests observe the
  // exact byte stream the device would persist without faking a
  // filesystem.
  void set_sink_for_test(std::function<void(const uint8_t *, std::size_t)> sink) {
    test_sink_ = std::move(sink);
  }

  // ---- Lifecycle (called by Texecom) -----------------------------------
  // Initialize the LittleFS mount + create the root_path directory.
  // Safe to call repeatedly; idempotent.
  void setup();

  // Begin a new capture file. Resets the relative microsecond clock and
  // emits a fresh TXCP header. Called from Texecom::on_new_client_().
  void on_session_start();

  // Close the current file (if any) and flush. Called from
  // Texecom::on_client_disconnect_().
  void on_session_end();

  // Periodic tick from Texecom::loop(). Drives the ring-buffer flush.
  // Bounded work — does not block the bridge.
  void loop();

  // Record `len` bytes flowing in `direction`. Returns true if accepted
  // into the ring buffer; false if dropped (ring full / filtered by mode
  // / disabled). NEVER blocks. Direction must be 0 or 1; out-of-range
  // values are treated as drops.
  bool record(uint8_t direction, const uint8_t *bytes, std::size_t len);

  // ---- Diagnostics -----------------------------------------------------
  Mode mode() const { return mode_; }
  const char *mode_str() const;
  std::size_t max_file_bytes() const { return max_file_bytes_; }
  const std::string &root_path() const { return root_path_; }
  uint32_t drops() const { return drops_; }
  uint32_t bytes_written() const { return bytes_written_total_; }
  bool is_session_active() const { return session_active_; }

  // True when the caller's mode + current session state would actually
  // persist a `record()` call. Lets Texecom skip the per-byte loop on
  // hot paths when capture is disabled.
  bool would_record() const {
    if (mode_ == Mode::None) return false;
    if (mode_ == Mode::BridgeOnly && !session_active_) return false;
    return true;
  }

 private:
  // Internal helpers — implementation in capture.cpp. Anything
  // touching LittleFS is guarded by USE_ARDUINO so host tests link.
  void open_new_file_();
  void close_current_file_();
  void write_bytes_(const uint8_t *bytes, std::size_t len);
  void write_hexdump_(uint64_t file_offset, const uint8_t *bytes, std::size_t len);
  void enqueue_event_(uint8_t direction, const uint8_t *bytes, std::size_t len);
  void flush_ring_();

  // Configuration.
  Mode mode_{Mode::BridgeOnly};
  std::string panel_name_{"Premier 24"};
  std::string root_path_{"/captures"};
  std::size_t max_file_bytes_{kDefaultCaptureMaxFileBytes};
  uint32_t baud_rate_{19200};

  // Test sink (set_sink_for_test). When non-null, persistence is
  // bypassed and the serialized stream is handed to the sink instead.
  std::function<void(const uint8_t *, std::size_t)> test_sink_{};

  // Runtime state.
  bool session_active_{false};
  bool fs_ready_{false};
  uint64_t session_start_us_{0};
  uint32_t file_seq_{0};
  uint32_t drops_{0};
  uint32_t bytes_written_total_{0};
  uint32_t current_file_bytes_{0};
  uint32_t warn_throttle_{0};

  // Per-event ring. We store the variable-length payload inline up to a
  // bounded size; larger pushes get split or dropped (Wintex frames stay
  // well under this).
  static constexpr std::size_t kMaxEventPayload = 256;
  struct PendingEvent {
    uint8_t direction;
    uint64_t ts_us;
    uint16_t len;
    uint8_t bytes[kMaxEventPayload];
  };
  std::array<PendingEvent, kCaptureRingEvents> ring_{};
  std::size_t ring_head_{0};
  std::size_t ring_tail_{0};
  std::size_t ring_size_{0};

  // Current file path (for HTTP listings / dump_config).
  std::string current_path_bin_{};
  std::string current_path_txt_{};
};

}  // namespace texecom
}  // namespace esphome
