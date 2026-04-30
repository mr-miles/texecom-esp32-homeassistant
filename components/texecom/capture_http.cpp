#include "capture_http.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// USE_NETWORK lives in esphome/core/defines.h (auto-generated per build).
// USE_ARDUINO is a -D flag set by PlatformIO. We pull defines.h in
// unconditionally so the gates below evaluate consistently in both this
// translation unit and the texecom.cpp caller. Inside an esp-idf-only
// host-test build defines.h would be absent, so we guard the include.
#if __has_include("esphome/core/defines.h")
#include "esphome/core/defines.h"
#endif

#if defined(USE_ARDUINO) && defined(USE_NETWORK)
#include <Arduino.h>
#include <esp_http_server.h>

#include <cstdio>
#include <vector>

#include "esphome/core/log.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include "capture.h"
#endif  // USE_ARDUINO && USE_NETWORK

namespace esphome {
namespace texecom {

// ---------- Pure, host-testable filename validator -------------------------

bool is_safe_capture_filename(const std::string &name) {
  // Empty / oversize names are out.
  if (name.empty()) return false;
  if (name.size() > 96) return false;

  // Reject anything starting with '.' (covers "." and "..", plus dotfiles).
  if (name.front() == '.') return false;

  // Scan every byte: no path separators, no NULs, no control chars.
  for (char c : name) {
    auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) return false;     // control chars / NUL
    if (c == '/' || c == '\\') return false;     // path separators
    if (c == ':') return false;                  // Windows drive specifier
  }

  // Extension whitelist — we only ever write .bin or .txt. Refusing others
  // both keeps the listing tidy and blocks attempts to fetch unrelated
  // state. The check is symmetric with the .bin/.txt content-type
  // selection in send_file_().
  if (name.size() < 5) return false;  // shortest legal: "x.bin"
  std::string ext = name.substr(name.size() - 4);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (ext != ".bin" && ext != ".txt") return false;

  return true;
}

// ---------- Arduino-only HTTP handler --------------------------------------

#if defined(USE_ARDUINO) && defined(USE_NETWORK)

namespace {

constexpr const char *const HTTP_TAG = "texecom.capture_http";

// Bytes per chunk for streamed download responses. 1 KB is a sweet spot
// for lwIP send-window pressure on the ESP32-S3.
constexpr std::size_t kStreamChunkBytes = 1024;

// Small HTML escape — only the four characters that matter for embedding
// untrusted filenames inside an HTML page. We keep it inline rather than
// pulling in any HTML library.
std::string html_escape_(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default:  out += c;        break;
    }
  }
  return out;
}

class CaptureHttpHandler : public AsyncWebHandler {
 public:
  CaptureHttpHandler(Capture *capture, std::string prefix)
      : capture_(capture), prefix_(std::move(prefix)) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET) return false;
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    auto url = request->url_to(url_buf);
    // Match `/captures` exactly (then we redirect) or any URL beneath it.
    if (url == prefix_) return true;
    if (url.size() < prefix_.size() + 1) return false;
    if (std::memcmp(url.c_str(), prefix_.c_str(), prefix_.size()) != 0) return false;
    return url[prefix_.size()] == '/';
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
    auto url_ref = request->url_to(url_buf);
    std::string url(url_ref);

    // /captures -> redirect to /captures/ so the relative download links
    // in the listing resolve correctly.
    if (url == prefix_) {
      request->redirect(prefix_ + "/");
      return;
    }

    // Strip the prefix + '/' to get the requested path-tail.
    std::string tail = url.substr(prefix_.size() + 1);

    // No tail = directory listing.
    if (tail.empty()) {
      send_listing_(request);
      return;
    }

    // URL-decoding has already happened in url_to(). Validate the
    // remaining component as a single safe filename — this rejects any
    // attempt to traverse, embed control bytes, or fetch non-capture files.
    if (!is_safe_capture_filename(tail)) {
      ESP_LOGW(HTTP_TAG, "rejected unsafe filename: '%s'", tail.c_str());
      request->send(404, "text/plain", "Not Found");
      return;
    }

    send_file_(request, tail);
  }

 private:
  void send_listing_(AsyncWebServerRequest *request) {
    auto entries = capture_->list_captures();
    std::sort(entries.begin(), entries.end(),
              [](const RamCaptureSnapshot &a, const RamCaptureSnapshot &b) {
                return a.name < b.name;
              });

    std::string body;
    body.reserve(2048);
    body += "<!doctype html>\n<html><head><meta charset=\"utf-8\">";
    body += "<title>Texecom captures</title>";
    body += "<style>body{font-family:sans-serif;margin:2em;}";
    body += "table{border-collapse:collapse;}";
    body += "th,td{padding:.3em .8em;text-align:left;border-bottom:1px solid #ccc;}";
    body += "th{background:#f4f4f4;}</style></head><body>";
    body += "<h1>Texecom captures</h1>";
    {
      char diag[128];
      std::snprintf(diag, sizeof(diag),
                    "<p>Storage: in-RAM (%u / %u bytes used)</p>",
                    (unsigned) capture_->total_bytes_used(),
                    (unsigned) capture_->max_total_bytes());
      body += diag;
    }

    if (entries.empty()) {
      body += "<p><em>No capture files yet.</em></p>";
    } else {
      body += "<table><tr><th>File</th><th>Size (bytes)</th>"
              "<th>Modified</th></tr>";
      uint32_t now_ms = (uint32_t) millis();
      for (const auto &e : entries) {
        body += "<tr><td><a href=\"";
        body += html_escape_(e.name);
        body += "\">";
        body += html_escape_(e.name);
        body += "</a></td><td>";
        body += std::to_string(e.size);
        body += "</td><td>";
        // No wall clock on this device — show "Ns ago" relative to now.
        // mtime_ms is millis() at last write.
        if (e.mtime_ms != 0 && now_ms >= e.mtime_ms) {
          uint32_t delta_s = (now_ms - e.mtime_ms) / 1000u;
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%us ago", (unsigned) delta_s);
          body += buf;
        } else {
          body += "&mdash;";
        }
        body += "</td></tr>";
      }
      body += "</table>";
    }
    body += "</body></html>";
    request->send(200, "text/html; charset=utf-8", body.c_str());
  }

  void send_file_(AsyncWebServerRequest *request, const std::string &name) {
    const std::vector<uint8_t> *bytes = capture_->get_capture_bytes(name);
    if (bytes == nullptr) {
      request->send(404, "text/plain", "Not Found");
      return;
    }

    bool is_bin = (name.size() >= 4 &&
                   std::strcmp(name.c_str() + name.size() - 4, ".bin") == 0);
    const char *content_type = is_bin ? "application/octet-stream"
                                      : "text/plain; charset=utf-8";

    httpd_req_t *req = static_cast<httpd_req_t *>(*request);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, content_type);
    char clen[24];
    std::snprintf(clen, sizeof(clen), "%u", (unsigned) bytes->size());
    httpd_resp_set_hdr(req, "Content-Length", clen);
    // .bin: prompt to save with the original filename. .txt: render inline.
    std::string disp;
    disp.reserve(name.size() + 32);
    disp += is_bin ? "attachment; filename=\"" : "inline; filename=\"";
    disp += name;
    disp += "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());

    // Stream out in fixed-size chunks so we don't pin a large send buffer.
    const uint8_t *src = bytes->data();
    std::size_t remaining = bytes->size();
    while (remaining > 0) {
      std::size_t n = remaining < kStreamChunkBytes ? remaining : kStreamChunkBytes;
      esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(src),
                                            n);
      if (err != ESP_OK) {
        ESP_LOGW(HTTP_TAG, "send_chunk failed err=%d on %s", (int) err, name.c_str());
        break;
      }
      src += n;
      remaining -= n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);  // terminate chunked response
  }

  Capture *capture_;
  std::string prefix_;
};

// Single instance per device. Allocated once on first call to
// register_capture_http_handler() and handed to WebServerBase, which owns
// the pointer for the rest of the process lifetime.
//
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
CaptureHttpHandler *g_handler = nullptr;

}  // namespace

void register_capture_http_handler(Capture *capture, const std::string &url_prefix) {
  if (g_handler != nullptr) {
    // Already registered (idempotent — Texecom::setup() may run twice
    // in some reload scenarios).
    return;
  }
  if (esphome::web_server_base::global_web_server_base == nullptr) {
    ESP_LOGE(HTTP_TAG, "WebServerBase not initialised; /captures route disabled");
    return;
  }
  std::string prefix = url_prefix.empty() ? std::string("/captures") : url_prefix;
  // Strip any trailing slash for canonical match — handler appends '/'.
  if (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();

  g_handler = new CaptureHttpHandler(capture, prefix);  // NOLINT
  esphome::web_server_base::global_web_server_base->add_handler(g_handler);
  ESP_LOGI(HTTP_TAG, "Capture HTTP handler registered at %s/", prefix.c_str());
}

#endif  // USE_ARDUINO && USE_NETWORK

}  // namespace texecom
}  // namespace esphome
