// Host-side unit tests for the Monitor/Bridge session state machine.
//
// SessionState is intentionally hardware-independent so it can be
// exercised here without AsyncServer / AsyncClient.

#include <catch2/catch_test_macros.hpp>

#include "session_state.h"

using esphome::texecom::SessionMode;
using esphome::texecom::SessionState;
using esphome::texecom::Transition;

TEST_CASE("SessionState: initial state is Monitor", "[session_state]") {
  SessionState s;
  REQUIRE(s.mode() == SessionMode::Monitor);
  REQUIRE_FALSE(s.is_bridge_mode());
  REQUIRE(s.active_client() == 0);
}

TEST_CASE("SessionState: connect transitions Monitor -> Bridge", "[session_state]") {
  SessionState s;
  auto t = s.on_connect(42);
  REQUIRE(t == Transition::EnteredBridge);
  REQUIRE(s.mode() == SessionMode::Bridge);
  REQUIRE(s.is_bridge_mode());
  REQUIRE(s.active_client() == 42);
}

TEST_CASE("SessionState: disconnect transitions Bridge -> Monitor", "[session_state]") {
  SessionState s;
  REQUIRE(s.on_connect(7) == Transition::EnteredBridge);
  auto t = s.on_disconnect(7);
  REQUIRE(t == Transition::EnteredMonitor);
  REQUIRE(s.mode() == SessionMode::Monitor);
  REQUIRE_FALSE(s.is_bridge_mode());
  REQUIRE(s.active_client() == 0);
}

TEST_CASE("SessionState: second connect while Bridge is rejected; state unchanged",
          "[session_state]") {
  SessionState s;
  REQUIRE(s.on_connect(1) == Transition::EnteredBridge);
  auto t = s.on_connect(2);
  REQUIRE(t == Transition::Rejected);
  REQUIRE(s.mode() == SessionMode::Bridge);
  REQUIRE(s.active_client() == 1);  // first client still owns the session
}

TEST_CASE("SessionState: on_accept_while_busy is pure/non-mutating", "[session_state]") {
  SessionState s;
  REQUIRE(s.on_accept_while_busy() == Transition::None);
  s.on_connect(10);
  REQUIRE(s.on_accept_while_busy() == Transition::Rejected);
  // State must be unchanged after the query.
  REQUIRE(s.mode() == SessionMode::Bridge);
  REQUIRE(s.active_client() == 10);
}

TEST_CASE("SessionState: disconnect from non-active client is a no-op",
          "[session_state]") {
  SessionState s;
  REQUIRE(s.on_connect(100) == Transition::EnteredBridge);
  // A rejected client's FIN should NOT tear down the active session.
  auto t = s.on_disconnect(999);
  REQUIRE(t == Transition::None);
  REQUIRE(s.mode() == SessionMode::Bridge);
  REQUIRE(s.active_client() == 100);
}

TEST_CASE("SessionState: reconnect after disconnect works", "[session_state]") {
  SessionState s;
  REQUIRE(s.on_connect(1) == Transition::EnteredBridge);
  REQUIRE(s.on_disconnect(1) == Transition::EnteredMonitor);
  REQUIRE(s.on_connect(2) == Transition::EnteredBridge);
  REQUIRE(s.active_client() == 2);
}

TEST_CASE("SessionState: reset returns to Monitor", "[session_state]") {
  SessionState s;
  s.on_connect(5);
  s.reset();
  REQUIRE(s.mode() == SessionMode::Monitor);
  REQUIRE(s.active_client() == 0);
}

TEST_CASE("SessionState: on_connect with reserved id 0 is a no-op", "[session_state]") {
  SessionState s;
  auto t = s.on_connect(0);
  REQUIRE(t == Transition::None);
  REQUIRE(s.mode() == SessionMode::Monitor);
}
