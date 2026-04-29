#pragma once

// HTTP exposure of the on-device capture directory (Plan 02-01 fix-up).
//
// Plan 02-01 shipped a LittleFS writer (capture.{h,cpp}) but its summary
// claimed downloads via web_server v3's "/file?file=..." endpoint. That
// endpoint does not exist in ESPHome's stock web_server — the SUMMARY was
// wrong. This module wires a small custom HTTP handler onto ESPHome's
// shared `web_server_base::WebServerBase` so the captures can actually be
// browsed and downloaded from a LAN browser.
//
// Endpoints (default URL prefix: /captures):
//   GET /captures            -> 302 redirect to /captures/
//   GET /captures/           -> HTML directory listing (filename, size,
//                               download links). No JS, no CSS framework.
//   GET /captures/<filename> -> Streams the file. .bin is sent with
//                               Content-Type: application/octet-stream and
//                               Content-Disposition: attachment so browsers
//                               prompt to save. .txt is text/plain inline so
//                               the user can read the hex dump directly.
//
// Path safety:
//   Only files directly under root_path_ (no nested directories) are
//   served. The filename is validated against `is_safe_capture_filename()`
//   below — that pure function rejects anything that could escape the
//   intended directory or smuggle control bytes into the filesystem call.
//   This is defence-in-depth on a LAN-only device; cheap to do right.
//
// Build gating:
//   The HTTP plumbing only compiles under USE_ARDUINO (Arduino-on-ESP32
//   framework) AND USE_NETWORK (web_server_base prerequisite). Host unit
//   tests compile this header for the pure path-safety function only — the
//   handler class is hidden behind the same guards as capture.cpp's
//   LittleFS code.

#include <cstddef>
#include <string>

namespace esphome {
namespace texecom {

class Capture;  // forward decl — defined in capture.h

// Pure, host-testable filename validator.
//
// Returns true iff `name` is safe to append to `<root>/` and serve over
// HTTP without enabling directory traversal or filesystem misuse. The
// rules:
//   - Non-empty
//   - No path separators ('/' or '\\')
//   - Does not start with '.' (rejects ".", "..", and dotfiles — keeps
//     the listing tidy and blocks the trivial traversal classics)
//   - No null bytes or other ASCII control characters (< 0x20 or 0x7F)
//   - Length <= 96 chars (matches the snprintf buffer in capture.cpp;
//     prevents long-name denial-of-service)
//   - Extension must be ".bin" or ".txt" (matches what we actually write)
//
// This is the only function in this header that the host test build
// links against. Everything else is gated behind the framework macros.
bool is_safe_capture_filename(const std::string &name);

// On-device only — register the /captures HTTP handler with ESPHome's
// shared web_server_base. Must be called once after the WebServerBase
// has been initialised (i.e. from Texecom::setup()). Idempotent.
//
// `capture` must outlive the handler (it does — Texecom owns both).
// `url_prefix` controls the URL root, e.g. "/captures" (no trailing slash).
#if defined(USE_ARDUINO) && defined(USE_NETWORK)
void register_capture_http_handler(Capture *capture, const std::string &url_prefix);
#endif

}  // namespace texecom
}  // namespace esphome
