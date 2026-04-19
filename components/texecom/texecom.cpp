#include "texecom.h"

#include "esphome/core/log.h"

#include "panel_model_premier24.h"

namespace esphome {
namespace texecom {

static const char *const TAG = "texecom";

// ---------- helpers ----------------------------------------------------------

#ifdef USE_ARDUINO
static std::string client_ip_(AsyncClient *c) {
  if (c == nullptr) {
    return "<null>";
  }
  IPAddress ip = c->remoteIP();
  char buf[24];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], c->remotePort());
  return std::string(buf);
}
#endif

// ---------- Component lifecycle ---------------------------------------------

void Texecom::setup() {
  // Default the panel model to Premier 24 if the YAML hasn't supplied
  // one. Phase 2 will let YAML pick the model explicitly.
  if (model_ == nullptr) {
    static PanelModelPremier24 default_model;
    model_ = &default_model;
  }

  uart_to_tcp_.clear();
  tcp_to_uart_.clear();
  uart_to_tcp_drops_ = 0;
  tcp_to_uart_paused_ticks_ = 0;
  session_.reset();
  active_client_id_ = kNoClient;

  // Capture: pick up panel name from the resolved model so dump_config
  // and the on-disk header agree on what panel produced the bytes.
  if (model_ != nullptr) {
    capture_.set_panel_name(model_->name());
  }
  capture_.setup();

#ifdef USE_ARDUINO
  server_ = new AsyncServer(tcp_port_);
  server_->onClient(
      [](void *arg, AsyncClient *c) {
        static_cast<Texecom *>(arg)->on_new_client_(c);
      },
      this);
  server_->begin();
  ESP_LOGI(TAG, "TCP listener started on port %u (panel=%s)", tcp_port_,
           model_ != nullptr ? model_->name() : "<none>");
#else
  ESP_LOGW(TAG, "USE_ARDUINO not defined — TCP listener disabled (host unit-test build?)");
#endif
}

void Texecom::loop() {
  // Cooperative byte-shuttling. No allocation inside this function —
  // both ring buffers live as members with std::array backing storage.
  pump_uart_to_tcp_();
  pump_tcp_to_uart_();
  // Drain any queued capture events to LittleFS. Bounded work; safe to
  // call every tick.
  capture_.loop();
}

void Texecom::dump_config() {
  ESP_LOGCONFIG(TAG, "Texecom:");
  ESP_LOGCONFIG(TAG, "  TCP port:   %u", tcp_port_);
  ESP_LOGCONFIG(TAG, "  Panel:      %s", model_ != nullptr ? model_->name() : "<unset>");
  if (model_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Zones:      %u", (unsigned) model_->zone_count());
    ESP_LOGCONFIG(TAG, "  Areas:      %u", (unsigned) model_->area_count());
  }
  ESP_LOGCONFIG(TAG, "  Session:    %s", is_bridge_mode() ? "Bridge" : "Monitor");
  ESP_LOGCONFIG(TAG, "  Buffers:    %u bytes each direction", (unsigned) kBufferSize);
  ESP_LOGCONFIG(TAG, "  Capture mode:  %s", capture_.mode_str());
  ESP_LOGCONFIG(TAG, "  Capture max:   %u bytes/file", (unsigned) capture_.max_file_bytes());
  ESP_LOGCONFIG(TAG, "  Capture root:  %s", capture_.root_path().c_str());
  ESP_LOGCONFIG(TAG, "  Capture drops: %u events lost", (unsigned) capture_.drops());
}

float Texecom::get_setup_priority() const {
  // After WiFi (which is AFTER_WIFI = 250) but before USER (1000).
  // The TCP listener needs network up; nothing else depends on this.
  return setup_priority::AFTER_WIFI;
}

// ---------- AsyncTCP callbacks ----------------------------------------------

#ifdef USE_ARDUINO

void Texecom::on_new_client_(AsyncClient *client) {
  if (client == nullptr) {
    return;
  }

  // Assign this connection an id BEFORE consulting session state so
  // on_disconnect matches correctly even for rejected clients.
  ClientId id = next_client_id_++;
  if (next_client_id_ == kNoClient) {
    next_client_id_ = 1;  // wrap, skipping reserved 0
  }

  Transition t = session_.on_connect(id);
  if (t == Transition::Rejected) {
    ESP_LOGW(TAG, "Rejecting second TCP client from %s (session already active)",
             client_ip_(client).c_str());
    client->close();
    // AsyncClient takes care of freeing on its own disconnect callback.
    return;
  }

  // Accepted — promote to active session.
  client_ = client;
  active_client_id_ = id;
  uart_to_tcp_.clear();
  tcp_to_uart_.clear();

  ESP_LOGI(TAG, "Session Monitor -> Bridge, client=%s", client_ip_(client).c_str());
  capture_.on_session_start();

  client->onData(
      [](void *arg, AsyncClient *c, void *data, size_t len) {
        static_cast<Texecom *>(arg)->on_client_data_(c, data, len);
      },
      this);
  client->onDisconnect(
      [](void *arg, AsyncClient *c) {
        static_cast<Texecom *>(arg)->on_client_disconnect_(c);
      },
      this);
  client->onError(
      [](void *arg, AsyncClient *c, int8_t e) {
        static_cast<Texecom *>(arg)->on_client_error_(c, e);
      },
      this);
  client->onTimeout(
      [](void *arg, AsyncClient *c, uint32_t) {
        static_cast<Texecom *>(arg)->on_client_disconnect_(c);
      },
      this);
}

void Texecom::on_client_data_(AsyncClient *client, void *data, size_t len) {
  if (client != client_) {
    // Stray callback from a rejected / stale client — ignore.
    return;
  }

  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  // Capture client->panel direction (1) BEFORE pushing into the ring so
  // we record the byte stream as it arrived from the wire — even if a
  // later byte gets back-pressured. Capture is best-effort; failures
  // never block the bridge.
  if (capture_.would_record() && len > 0) {
    capture_.record(/*direction=*/1, bytes, len);
  }
  for (size_t i = 0; i < len; ++i) {
    if (!tcp_to_uart_.push(bytes[i])) {
      // Back-pressure: do NOT drop config-write bytes. Stop ACKing until
      // loop() drains. AsyncTCP doesn't expose a clean "pause" on
      // esp-idf, but ack'ing less-than-received throttles the peer.
      ++tcp_to_uart_paused_ticks_;
      if (client_ != nullptr) {
        // Ack only the bytes we managed to buffer.
        client_->ack(i);
      }
      return;
    }
  }
  // All bytes accepted — ack the full length so the TCP window reopens.
  if (client_ != nullptr) {
    client_->ack(len);
  }
}

void Texecom::on_client_disconnect_(AsyncClient *client) {
  // Use the id we stashed at accept time, not whatever `client` is now —
  // it may already be torn down.
  Transition t = session_.on_disconnect(active_client_id_);
  if (t == Transition::EnteredMonitor) {
    ESP_LOGI(TAG, "Session Bridge -> Monitor (client disconnected)");
    capture_.on_session_end();
  }
  // Flush remaining UART->TCP bytes to the panel side? No — direction
  // is UART->TCP; on client gone we just discard. Panel-bound writes
  // already in tcp_to_uart_ get drained into the UART by loop().
  client_ = nullptr;
  active_client_id_ = kNoClient;
  (void) client;
}

void Texecom::on_client_error_(AsyncClient *client, int8_t error) {
  ESP_LOGW(TAG, "TCP client error %d from %s", (int) error,
           client_ip_(client).c_str());
  on_client_disconnect_(client);
}

#endif  // USE_ARDUINO

// ---------- loop() pumps ----------------------------------------------------

void Texecom::pump_uart_to_tcp_() {
  if (uart_ == nullptr) {
    return;
  }

  // 1. Drain the UART RX into the outbound ring buffer.
  //    drop-oldest overflow policy: Wintex will retry on timeout, so
  //    keeping the freshest bytes is better than wedging.
  //
  //    We coalesce the UART bytes into a small stack buffer so the
  //    capture call sees a single contiguous record per loop tick
  //    rather than one event per byte (a 64-byte Wintex frame becomes
  //    1 capture event instead of 64).
  uint8_t rx_batch[256];
  std::size_t rx_n = 0;
  while (uart_->available() && rx_n < sizeof(rx_batch)) {
    uint8_t byte = 0;
    if (!uart_->read_byte(&byte)) {
      break;
    }
    rx_batch[rx_n++] = byte;
    if (!uart_to_tcp_.push(byte)) {
      uart_to_tcp_.drop_oldest();
      (void) uart_to_tcp_.push(byte);
      ++uart_to_tcp_drops_;
      if ((uart_to_tcp_drops_ & 0xFF) == 1) {
        ESP_LOGW(TAG, "UART->TCP buffer overflow (%u total drops)",
                 (unsigned) uart_to_tcp_drops_);
      }
    }
  }
  // Record panel->client direction (0). would_record() short-circuits
  // when capture is disabled / wrong-mode so the cost is negligible.
  if (rx_n > 0 && capture_.would_record()) {
    capture_.record(/*direction=*/0, rx_batch, rx_n);
  }

#ifdef USE_ARDUINO
  // 2. Push as much of the ring as the TCP send window will accept.
  //    Only stage what we know will fit — that way we never have to
  //    un-pop bytes and there's no risk of reordering with bytes the
  //    UART drains on the NEXT loop tick.
  if (client_ == nullptr || !client_->connected() || uart_to_tcp_.empty()) {
    return;
  }

  size_t space = client_->space();
  if (space == 0) {
    return;  // TCP window full; try next tick.
  }

  static constexpr size_t kStageSize = 256;
  uint8_t stage[kStageSize];
  size_t budget = space < kStageSize ? space : kStageSize;
  size_t n = 0;
  while (n < budget) {
    uint8_t b = 0;
    if (!uart_to_tcp_.pop(b)) {
      break;
    }
    stage[n++] = b;
  }
  if (n == 0) {
    return;
  }
  client_->write(reinterpret_cast<const char *>(stage), n);
  client_->send();
#endif
}

void Texecom::pump_tcp_to_uart_() {
  if (uart_ == nullptr) {
    return;
  }
  // Drain tcp_to_uart_ into the UART TX. write_byte() buffers inside
  // the ESPHome UART driver; we push until either the ring is empty or
  // the UART reports it can't accept more.
  uint8_t b = 0;
  while (tcp_to_uart_.peek(b)) {
    uart_->write_byte(b);
    tcp_to_uart_.pop(b);
  }
}

}  // namespace texecom
}  // namespace esphome
