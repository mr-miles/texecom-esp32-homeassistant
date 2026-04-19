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

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/uart/uart.h"

#include "capture.h"
#include "panel_model.h"
#include "ring_buffer.h"
#include "session_state.h"

// AsyncTCP is provided via the AsyncTCP library declared in the ESPHome
// YAML. It is an Arduino-framework library; the texecom component
// therefore requires `framework: arduino` (set in the YAML).
#ifdef USE_ARDUINO
#include <AsyncTCP.h>
#endif

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
  void set_capture_max_file_bytes(uint32_t n) { capture_.set_max_file_bytes(n); }
  void set_capture_root(const std::string &p) { capture_.set_root_path(p); }

  // ESPHome Component lifecycle.
  void setup() override;
  void loop() override;
  void dump_config() override;
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
#ifdef USE_ARDUINO
  // AsyncTCP event plumbing.
  void on_new_client_(AsyncClient *client);
  void on_client_data_(AsyncClient *client, void *data, size_t len);
  void on_client_disconnect_(AsyncClient *client);
  void on_client_error_(AsyncClient *client, int8_t error);
#endif

  // loop() helpers.
  void pump_uart_to_tcp_();
  void pump_tcp_to_uart_();

  // Config.
  uart::UARTComponent *uart_{nullptr};
  PanelModel *model_{nullptr};
  uint16_t tcp_port_{10001};

  // Runtime.
#ifdef USE_ARDUINO
  AsyncServer *server_{nullptr};
  AsyncClient *client_{nullptr};
#endif
  SessionState session_{};
  ClientId next_client_id_{1};  // 0 is reserved for "no client"
  ClientId active_client_id_{kNoClient};

  // Backpressure ring buffers (stack/static — zero heap on the hot path).
  RingBuffer<uint8_t, kBufferSize> uart_to_tcp_{};
  RingBuffer<uint8_t, kBufferSize> tcp_to_uart_{};

  // Diagnostic counters (reset on setup).
  uint32_t uart_to_tcp_drops_{0};
  uint32_t tcp_to_uart_paused_ticks_{0};

  // Capture sink (Plan 02-01). Owns its own LittleFS file handles.
  Capture capture_{};
};

}  // namespace texecom
}  // namespace esphome
