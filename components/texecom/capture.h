#pragma once

// On-device capture of bytes flowing through the Texecom bridge.
//
// Plan 02-01 deliverable. Records every byte the bridge moves in either
// direction with a relative microsecond timestamp. Files are downloadable
// over HTTP via the ESPHome web_server; the binary format is documented
// below and host-side tested via `tests/test_capture.cpp` so the parser
// used in Plan 02-02 has a guaranteed wire-format contract.
//
// =====================================================================
// Storage model: in-RAM (no LittleFS)
// =====================================================================
//
// The original implementation persisted captures to LittleFS. On this
// Atom S3 partition the arduino-esp32 bundled LittleFS and joltwallet's
// esp_littlefs (used by web_server_idf) don't share state — writes
// reported success but `LittleFS.exists()` returned false immediately
// afterwards, with the same behaviour after a full `esptool erase_flash`
// + USB reflash. Since the workflow is "run a Wintex session ->
// immediately download from the device's web UI", cross-reboot
// persistence is not required: captures live in heap RAM and are evicted
// oldest-first when a configurable byte budget is exceeded.
//
// =====================================================================
// TXCP binary format v1 (LOCKED — do not change without bumping version)
// =====================================================================
//
// Capture layout: <header><event><event>...<event>
//
// File header — exactly 32 bytes, written once per capture:
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
// Per-session capture pair:
//   * Each Wintex session produces one .bin (the TXCP byte stream) and
//     one .txt (xxd-style hex sidecar that renders inline in the browser).
//   * Filename: "wintex-{unix_ts}-{seq}.bin" if SNTP synced, else
//     "wintex-boot{boot_ms}-{seq}.bin". Sequence counter avoids
//     collisions inside one boot.
//
// Budget eviction:
//   * Total RAM used by all stored captures is bounded by
//     `max_total_bytes_` (default 128 KB). When a write would push the
//     total over budget, the OLDEST capture is evicted first. The
//     currently-live .bin / .txt are skipped — truncating the active
//     session would corrupt mid-flight events.
//
// Backpressure:
//   * In-memory ring buffer of N events (default 64). If full, the new
//     event is dropped and `drops_` is incremented; a single WARN log
//     fires per overflow burst. The bridge NEVER blocks on capture.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace esphome {
namespace texecom {

// Locked format constants — do not edit.
static constexpr std::size_t kCaptureHeaderSize = 32;
static constexpr std::size_t kCaptureEventFixedSize = 11;  // direction + ts + len
static constexpr uint16_t kCaptureFormatVersion = 1;
static constexpr uint8_t kCaptureMagic[4] = {'T', 'X', 'C', 'P'};
static constexpr std::size_t kCapturePanelNameLen = 16;
static constexpr std::size_t kDefaultCaptureMaxRamBytes = 131072;  // 128 KB
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

// Snapshot view of a stored capture, returned by list_captures(). Copy
// of the metadata only — does not alias internal storage.
struct RamCaptureSnapshot {
  std::string name;     // basename, e.g. "wintex-boot1234-001.bin"
  std::size_t size;     // bytes currently in this capture
  uint32_t mtime_ms;    // millis() at last write — best-effort, no wall-clock
};

// Capture sink: owns the per-event ring buffer and the in-RAM capture
// store. Values returned by `get_capture_bytes()` reference internal
// vectors and are invalidated by any mutating call (record(),
// on_session_start(), eviction).
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
  void set_max_total_bytes(std::size_t bytes) {
    max_total_bytes_ = bytes ? bytes : kDefaultCaptureMaxRamBytes;
  }
  void set_baud_rate(uint32_t baud) { baud_rate_ = baud; }

  // Test/host hook: when set, serialized bytes are sent to this sink
  // INSTEAD OF being appended to the in-RAM capture. Lets host tests
  // observe the exact byte stream the device would persist without
  // exercising the eviction logic.
  void set_sink_for_test(std::function<void(const uint8_t *, std::size_t)> sink) {
    test_sink_ = std::move(sink);
  }

  // ---- Lifecycle (called by Texecom) -----------------------------------
  // No filesystem to mount — just logs the configured budget.
  void setup();

  // Begin a new capture. Pushes a fresh .bin/.txt pair into the in-RAM
  // store, writes the header banner, and resets the relative-time clock.
  // Called from Texecom::on_new_client_().
  void on_session_start();

  // Flush any queued events and mark the live capture closed (it stays
  // in the store, available for download). Called from
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
  std::size_t max_total_bytes() const { return max_total_bytes_; }
  std::size_t total_bytes_used() const { return total_bytes_used_; }
  uint32_t drops() const { return drops_; }
  uint32_t bytes_written() const { return bytes_written_total_; }
  bool is_session_active() const { return session_active_; }

  // Snapshot of every capture currently held in RAM. Cheap to call —
  // returns copies; safe to use from the HTTP handler thread.
  std::vector<RamCaptureSnapshot> list_captures() const;

  // Lookup the raw byte vector for a capture by basename. Returns
  // nullptr if not found. The returned pointer is valid until the next
  // call that mutates `captures_` (record, on_session_start, eviction).
  const std::vector<uint8_t> *get_capture_bytes(const std::string &name) const;

  // True when the caller's mode + current session state would actually
  // persist a `record()` call. Lets Texecom skip the per-byte loop on
  // hot paths when capture is disabled.
  bool would_record() const {
    if (mode_ == Mode::None) return false;
    if (mode_ == Mode::BridgeOnly && !session_active_) return false;
    return true;
  }

 private:
  // In-RAM capture entry. Each session produces a .bin and a paired .txt.
  struct RamCapture {
    std::string name;             // basename
    std::vector<uint8_t> bytes;   // header + serialized events, OR hex-dump text
    uint32_t mtime_ms{0};
    bool is_text{false};
  };

  // Internal helpers.
  void open_new_capture_();
  void close_current_capture_();
  void write_bytes_(const uint8_t *bytes, std::size_t len);
  void write_hexdump_(uint64_t file_offset, const uint8_t *bytes, std::size_t len);
  void enqueue_event_(uint8_t direction, const uint8_t *bytes, std::size_t len);
  void flush_ring_();
  // Evict oldest captures until total_bytes_used_ <= max_total_bytes_.
  // Skips the live .bin / .txt — if those alone exceed the budget, logs
  // a single WARN and returns without truncating mid-session.
  void enforce_budget_();
  // Append `len` bytes to capture index `idx` and update accounting.
  // Caller must hold a valid index.
  void append_to_(int idx, const uint8_t *bytes, std::size_t len);

  // Configuration.
  Mode mode_{Mode::BridgeOnly};
  std::string panel_name_{"Premier 24"};
  std::size_t max_total_bytes_{kDefaultCaptureMaxRamBytes};
  uint32_t baud_rate_{19200};

  // Test sink (set_sink_for_test). When non-null, persistence is
  // bypassed and the serialized stream is handed to the sink instead.
  std::function<void(const uint8_t *, std::size_t)> test_sink_{};

  // Runtime state.
  bool session_active_{false};
  uint64_t session_start_us_{0};
  uint32_t file_seq_{0};
  uint32_t drops_{0};
  uint32_t bytes_written_total_{0};
  uint32_t warn_throttle_{0};
  bool budget_warn_emitted_{false};

  // In-RAM capture store. Index-based to keep current_*_idx_ pointers
  // stable across pushes; eviction adjusts the indices in lock-step.
  std::vector<RamCapture> captures_{};
  std::size_t total_bytes_used_{0};
  int current_bin_idx_{-1};
  int current_txt_idx_{-1};

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
};

}  // namespace texecom
}  // namespace esphome
