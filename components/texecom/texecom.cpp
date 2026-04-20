#include "texecom.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <span>

#include "esphome/core/log.h"
#include "esphome/components/socket/headers.h"
#include "esphome/components/socket/socket.h"

#include "panel_model_premier24.h"

namespace esphome {
namespace texecom {

static const char *const TAG = "texecom";

// ---------- helpers ----------------------------------------------------------

// Format a peer sockaddr into `out` (fixed-size buffer of exactly
// socket::SOCKADDR_STR_LEN chars). Always null-terminates. Falls back
// to "<unknown>" on error.
static void format_peer_(socket::Socket *sock,
                         std::span<char, socket::SOCKADDR_STR_LEN> out) {
  if (sock == nullptr) {
    std::strncpy(out.data(), "<unknown>", out.size() - 1);
    out[out.size() - 1] = '\0';
    return;
  }
  std::size_t n = sock->getpeername_to(out);
  if (n == 0 || n >= out.size()) {
    std::strncpy(out.data(), "<unknown>", out.size() - 1);
    out[out.size() - 1] = '\0';
  } else {
    out[n] = '\0';
  }
}

// Is `errno` a non-fatal "would block"? lwip/BSD use EAGAIN /
// EWOULDBLOCK; some stacks also surface EINPROGRESS for accept.
static inline bool would_block_() {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

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
  send_stage_len_ = 0;
  send_stage_off_ = 0;
  uart_to_tcp_drops_ = 0;
  tcp_to_uart_paused_ticks_ = 0;
  session_.reset();
  active_client_id_ = kNoClient;
  next_client_id_ = 1;

  // Capture: pick up panel name from the resolved model so dump_config
  // and the on-disk header agree on what panel produced the bytes.
  if (model_ != nullptr) {
    capture_.set_panel_name(model_->name());
  }
  capture_.setup();

  // Create the TCP listener. `socket_listen()` returns a ListenSocket,
  // which on the LWIP_TCP backend is a distinct type from Socket (the
  // raw-TCP connected class can't listen). AF_INET is explicit here
  // because Wintex is IPv4-only on the LAN segments we deploy to; if
  // we ever need IPv6 here we can check USE_NETWORK_IPV6.
  listener_ = socket::socket_listen(AF_INET, SOCK_STREAM, 0);
  if (listener_ == nullptr) {
    ESP_LOGE(TAG, "Could not create listening socket");
    this->mark_failed();
    return;
  }

  int err = listener_->setblocking(false);
  if (err != 0) {
    ESP_LOGW(TAG, "setblocking(false) failed: errno=%d", errno);
  }

  int enable = 1;
  err = listener_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  if (err != 0) {
    ESP_LOGW(TAG, "SO_REUSEADDR failed: errno=%d", errno);
  }

  // Bind AF_INET any-address. We construct sockaddr_in directly rather
  // than using socket::set_sockaddr_any(), because that helper picks
  // AF_INET6 on builds that enable IPv6 — which would fail to bind to
  // the AF_INET listener we just created above.
  struct sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(ESPHOME_INADDR_ANY);
  bind_addr.sin_port = htons(tcp_port_);
  err = listener_->bind(reinterpret_cast<struct sockaddr *>(&bind_addr), sizeof(bind_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "bind(port=%u) failed: errno=%d", tcp_port_, errno);
    this->mark_failed();
    return;
  }

  // backlog=1 — single-client policy means we never queue beyond the
  // one currently-being-rejected connection.
  err = listener_->listen(1);
  if (err != 0) {
    ESP_LOGE(TAG, "listen() failed: errno=%d", errno);
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "TCP listener started on port %u (panel=%s)", tcp_port_,
           model_ != nullptr ? model_->name() : "<none>");
}

void Texecom::loop() {
  // Cooperative byte-shuttling. No allocation inside these helpers —
  // both ring buffers live as members with std::array backing storage,
  // and the stack staging buffers are fixed-size.
  accept_pending_client_();
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

void Texecom::on_shutdown() {
  // Close the client first (so the peer sees a clean FIN), then the
  // listener. Both unique_ptr<Socket> destructors also close on reset()
  // but closing explicitly here makes the sequence easy to reason about.
  if (client_socket_) {
    client_socket_->close();
    client_socket_.reset();
  }
  if (listener_) {
    listener_->close();
    listener_.reset();
  }
}

float Texecom::get_setup_priority() const {
  // After WiFi (which is AFTER_WIFI = 250) but before USER (1000).
  // The TCP listener needs network up; nothing else depends on this.
  return setup_priority::AFTER_WIFI;
}

// ---------- accept / session lifecycle --------------------------------------

void Texecom::reject_client_(std::unique_ptr<socket::Socket> pending,
                             const char *peer_str) {
  ESP_LOGW(TAG, "Rejecting second TCP client from %s (session already active)",
           peer_str);
  if (pending) {
    pending->close();
  }
}

void Texecom::accept_pending_client_() {
  if (!listener_) {
    return;
  }

  // We use getpeername_to() on the accepted socket to format the peer
  // address; no need to have accept() fill the sockaddr as well.
  std::unique_ptr<socket::Socket> pending = listener_->accept(nullptr, nullptr);
  if (!pending) {
    // No pending connection (EAGAIN) or transient failure — retry next
    // tick. The socket layer already logs spurious errors.
    return;
  }

  std::array<char, socket::SOCKADDR_STR_LEN> peer_buf{};
  format_peer_(pending.get(),
               std::span<char, socket::SOCKADDR_STR_LEN>{peer_buf.data(), peer_buf.size()});
  const char *peer_str = peer_buf.data();

  // Assign an id BEFORE consulting session state so any subsequent
  // disconnect matches correctly even for rejected clients. Skip id=0
  // (reserved for kNoClient).
  ClientId id = next_client_id_++;
  if (next_client_id_ == kNoClient) {
    next_client_id_ = 1;
  }

  Transition t = session_.on_connect(id);
  if (t == Transition::Rejected) {
    reject_client_(std::move(pending), peer_str);
    return;
  }

  // Accepted — promote to active session.
  int err = pending->setblocking(false);
  if (err != 0) {
    ESP_LOGW(TAG, "Client setblocking(false) failed: errno=%d", errno);
  }
  // Disable Nagle so Wintex request/response packets ship immediately
  // instead of coalescing into 40 ms delays. Wintex is a chatty
  // synchronous protocol, so this is a net win.
  int enable = 1;
  (void) pending->setsockopt(IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));

  client_socket_ = std::move(pending);
  active_client_id_ = id;
  uart_to_tcp_.clear();
  tcp_to_uart_.clear();
  send_stage_len_ = 0;
  send_stage_off_ = 0;

  ESP_LOGI(TAG, "Session Monitor -> Bridge, client=%s", peer_str);
  capture_.on_session_start();
}

void Texecom::end_session_(const char *reason) {
  if (client_socket_) {
    client_socket_->close();
    client_socket_.reset();
  }
  Transition t = session_.on_disconnect(active_client_id_);
  if (t == Transition::EnteredMonitor) {
    ESP_LOGI(TAG, "Session Bridge -> Monitor (%s)", reason);
    capture_.on_session_end();
  }
  active_client_id_ = kNoClient;
  // Drop any UART->TCP send-stage bytes — they were destined for the
  // departed client and should not be replayed to a future session.
  send_stage_len_ = 0;
  send_stage_off_ = 0;
  // Do NOT clear the ring buffers — panel-bound bytes already staged
  // in tcp_to_uart_ get drained on subsequent ticks regardless.
}

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

  // 2. Ring -> socket. Only runs in Bridge mode with an active client.
  if (!is_bridge_mode() || !client_socket_) {
    return;
  }

  // We keep a persistent stage buffer (`send_stage_`) that holds any
  // tail bytes the socket refused on the previous tick. This avoids
  // needing a "push to front" primitive on the ring — ordering is
  // preserved because the stage is always fully drained before we
  // pull more bytes from the ring.
  while (true) {
    // Refill the stage from the ring if it is empty.
    if (send_stage_off_ >= send_stage_len_) {
      send_stage_off_ = 0;
      send_stage_len_ = 0;
      while (send_stage_len_ < send_stage_.size()) {
        uint8_t b = 0;
        if (!uart_to_tcp_.pop(b)) {
          break;
        }
        send_stage_[send_stage_len_++] = b;
      }
      if (send_stage_len_ == 0) {
        return;  // ring empty and nothing pending — done.
      }
    }

    const uint8_t *ptr = send_stage_.data() + send_stage_off_;
    std::size_t remaining = send_stage_len_ - send_stage_off_;
    // Use write() (not send()) — it is present on all three ESPHome
    // socket backends (BSD, LwIP sockets, LwIP raw TCP). The client
    // socket is non-blocking (setblocking(false) at accept), so write()
    // returns immediately on a full send buffer.
    ssize_t sent = client_socket_->write(ptr, remaining);
    if (sent < 0) {
      if (would_block_()) {
        // Socket send buffer is full — keep stage bytes for next tick.
        return;
      }
      ESP_LOGW(TAG, "write() failed: errno=%d, ending session", errno);
      end_session_("write error");
      return;
    }

    send_stage_off_ += (std::size_t) sent;
    if ((std::size_t) sent < remaining) {
      // Short send — socket window is full; retry next tick.
      return;
    }
    // Full send — loop and refill stage from the ring if more is waiting.
  }
}

void Texecom::pump_tcp_to_uart_() {
  if (uart_ == nullptr) {
    return;
  }

  // 1. Socket -> ring. Only read if the ring has room (never drop
  //    panel-bound bytes — corrupting a config write would wedge the
  //    panel). When the ring is full we stop calling recv(); the TCP
  //    receive window will close naturally and throttle the peer.
  if (is_bridge_mode() && client_socket_) {
    while (!tcp_to_uart_.full()) {
      uint8_t buf[256];
      // Read at most the ring's remaining space so we never need to
      // drop a byte we already pulled off the socket.
      std::size_t room = kBufferSize - tcp_to_uart_.size();
      std::size_t want = room < sizeof(buf) ? room : sizeof(buf);
      ssize_t n = client_socket_->read(buf, want);
      if (n == 0) {
        // Peer closed cleanly.
        end_session_("client disconnected");
        return;
      }
      if (n < 0) {
        if (would_block_()) {
          break;  // No data available right now.
        }
        ESP_LOGW(TAG, "recv() failed: errno=%d, ending session", errno);
        end_session_("recv error");
        return;
      }

      for (ssize_t i = 0; i < n; ++i) {
        (void) tcp_to_uart_.push(buf[i]);  // ring had room (checked above)
      }
      if (capture_.would_record()) {
        capture_.record(/*direction=*/1, buf, (std::size_t) n);
      }
    }
    if (tcp_to_uart_.full()) {
      ++tcp_to_uart_paused_ticks_;
    }
  }

  // 2. Ring -> UART. write_byte() buffers inside the ESPHome UART
  //    driver; we push until either the ring is empty or (by design)
  //    the driver absorbs it all. ESPHome's UART::write_byte is
  //    best-effort — there is no backpressure path from the driver.
  uint8_t b = 0;
  while (tcp_to_uart_.peek(b)) {
    uart_->write_byte(b);
    tcp_to_uart_.pop(b);
  }
}

}  // namespace texecom
}  // namespace esphome
