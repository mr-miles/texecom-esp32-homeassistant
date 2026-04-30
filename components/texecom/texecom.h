#pragma once

// ESPHome external component: texecom.
//
// Wires a UARTComponent (connected to the Premier 24 COM1 header) to a
// TCP listener on a configurable port (default 10001). Single Wintex
// client at a time; a second connection attempt is rejected cleanly.
//
// Phase 1 responsibility: transparent byte pipe + Monitor/Bridge state
// tracking. Phase 2 adds protocol decode. Phase 3 gates MQTT publishing
// on `is_bridge_mode()`.
//
// Transport: ESPHome's own `socket::Socket` API (see
// `esphome/components/socket/socket.h`). Callbacks are NOT used — the
// listener and client socket are polled cooperatively from `loop()`.
// This replaces the Plan 01-02 callback-driven implementation, whose
// external library pin kept breaking across Arduino-ESP32 revisions.

#include <array>
#include <cstddef>
#include <memory>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/socket/socket.h"

#include "capture.h"
#include "panel_model.h"
#include "ring_buffer.h"
#include "session_state.h"

namespace esphome {
namespace texecom {

// Ring buffer sizes. 1024 bytes each direction is plenty of head-room
// for Wintex frames (largest observed payload ~512B) at 19200 baud.
static constexpr std::size_t kBufferSize = 1024;

class Texecom : public Component {
 public:
  Texecom() = default;

  // Config setters (wired from __init__.py).
  void set_uart_parent(uart::UARTComponent *parent) { uart_ = parent; }
  void set_tcp_port(uint16_t port) { tcp_port_ = port; }
  void set_panel_model(PanelModel *model) { model_ = model; }

  // Capture configuration (Plan 02-01). All YAML-driven; safe to call
  // multiple times before setup().
  void set_capture_mode(Capture::Mode m) { capture_.set_mode(m); }
  void set_capture_max_ram_bytes(uint32_t n) { capture_.set_max_total_bytes(n); }

  // ESPHome Component lifecycle.
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override;

  // Public accessors.
  //
  // Phase 3 (MQTT) will consume `is_bridge_mode()` to gate publishing
  // while a Wintex client has the UART. DO NOT remove this API without
  // updating the Phase 3 plan.
  bool is_bridge_mode() const { return session_.is_bridge_mode(); }
  PanelModel *panel_model() const { return model_; }
  uint16_t tcp_port() const { return tcp_port_; }

 protected:
  // loop() helpers. Called in order every tick.
  void accept_pending_client_();
  void pump_uart_to_tcp_();
  void pump_tcp_to_uart_();

  // Teardown helper — called when recv() returns 0/error or by
  // on_shutdown(). Idempotent.
  void end_session_(const char *reason);

  // Reject the given pending socket (second-client policy). The socket
  // is closed and dropped; the session state is NOT mutated.
  void reject_client_(std::unique_ptr<socket::Socket> pending,
                      const char *peer_str);

  // Config.
  uart::UARTComponent *uart_{nullptr};
  PanelModel *model_{nullptr};
  uint16_t tcp_port_{10001};

  // Runtime — ESPHome socket handles.
  std::unique_ptr<socket::ListenSocket> listener_{};
  std::unique_ptr<socket::Socket> client_socket_{};

  SessionState session_{};
  ClientId next_client_id_{1};  // 0 is reserved for "no client"
  ClientId active_client_id_{kNoClient};

  // Backpressure ring buffers (stack/static — zero heap on the hot path).
  RingBuffer<uint8_t, kBufferSize> uart_to_tcp_{};
  RingBuffer<uint8_t, kBufferSize> tcp_to_uart_{};

  // Persistent send stage for UART->TCP. Holds any tail bytes that the
  // socket refused on the previous tick (short send / EAGAIN). Must be
  // flushed before staging new bytes from `uart_to_tcp_` to preserve
  // byte ordering.
  static constexpr std::size_t kSendStageSize = 256;
  std::array<uint8_t, kSendStageSize> send_stage_{};
  std::size_t send_stage_len_{0};
  std::size_t send_stage_off_{0};  // bytes already sent from send_stage_

  // Diagnostic counters (reset on setup).
  uint32_t uart_to_tcp_drops_{0};
  uint32_t tcp_to_uart_paused_ticks_{0};

  // Capture sink (Plan 02-01). In-RAM storage with a configurable byte
  // budget; oldest-first eviction. See capture.h for the full design.
  Capture capture_{};
};

}  // namespace texecom
}  // namespace esphome
