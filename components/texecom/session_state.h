#pragma once

// Pure, hardware-independent session state machine for the Texecom bridge.
//
// Kept separate from the Texecom ESPHome component so that host-side unit
// tests can exercise the transitions without AsyncServer / AsyncClient.
//
// Model:
//   * Monitor  — no TCP client connected. MQTT publish path is live
//                (Phase 3). UART packets can be decoded and surfaced
//                as events.
//   * Bridge   — one TCP client is connected. Raw byte pipe between
//                that client and the UART. MQTT publish paused.
//
// Policy:
//   * First client to connect wins.
//   * A second connection attempt while in Bridge returns
//     Transition::Rejected and the state stays at Bridge.
//   * `on_disconnect` only transitions if the disconnecting client id
//     matches the active session; spurious disconnect callbacks are
//     ignored.

#include <cstdint>

namespace esphome {
namespace texecom {

enum class SessionMode : uint8_t {
  Monitor = 0,
  Bridge = 1,
};

// Return value from the transition methods. `Rejected` means "no change;
// the attempt was refused" (used by on_connect when already in Bridge).
enum class Transition : uint8_t {
  None = 0,          // No state change (call was a no-op)
  EnteredBridge,     // Monitor -> Bridge
  EnteredMonitor,    // Bridge  -> Monitor
  Rejected,          // Attempt to connect while already in Bridge
};

// `ClientId` is an opaque handle; the caller assigns unique non-zero ids
// to each connection so we can distinguish "disconnect from active
// client" from "disconnect from a previously-rejected client". 0 means
// "no active client".
using ClientId = uint32_t;
static constexpr ClientId kNoClient = 0;

class SessionState {
 public:
  SessionState() = default;

  SessionMode mode() const { return mode_; }
  ClientId active_client() const { return active_; }
  bool is_bridge_mode() const { return mode_ == SessionMode::Bridge; }

  // Called when a new client connection is accepted.
  //   - If currently Monitor: transition to Bridge, record `id`.
  //   - If currently Bridge : refuse (return Rejected); state unchanged.
  Transition on_connect(ClientId id) {
    if (id == kNoClient) {
      return Transition::None;  // defensive: 0 reserved
    }
    if (mode_ == SessionMode::Bridge) {
      return Transition::Rejected;
    }
    mode_ = SessionMode::Bridge;
    active_ = id;
    return Transition::EnteredBridge;
  }

  // Called when a client disconnects.
  //   - If `id` matches active client: transition to Monitor.
  //   - Otherwise: no-op (the rejected client's FIN fires too).
  Transition on_disconnect(ClientId id) {
    if (mode_ != SessionMode::Bridge) {
      return Transition::None;
    }
    if (id != active_) {
      return Transition::None;
    }
    mode_ = SessionMode::Monitor;
    active_ = kNoClient;
    return Transition::EnteredMonitor;
  }

  // Explicit convenience: what would happen if we accepted while busy?
  // (Useful for tests and for logging the rejected IP without mutating
  // state.)
  Transition on_accept_while_busy() const {
    return mode_ == SessionMode::Bridge ? Transition::Rejected : Transition::None;
  }

  void reset() {
    mode_ = SessionMode::Monitor;
    active_ = kNoClient;
  }

 private:
  SessionMode mode_{SessionMode::Monitor};
  ClientId active_{kNoClient};
};

}  // namespace texecom
}  // namespace esphome
