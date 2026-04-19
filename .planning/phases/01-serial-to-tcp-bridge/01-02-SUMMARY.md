# Plan 01-02 — ESPHome External Component (TCP↔UART Bridge) — Summary

## What shipped

### Component (`components/texecom/`)
- **`__init__.py`** — ESPHome code-gen with config schema (`id`, `uart_id` required, `tcp_port` default 10001). Adds `AsyncTCP` as a library dep via `cg.add_library`.
- **`ring_buffer.h`** — header-only `RingBuffer<T, N>` template (SPSC, fixed `std::array` storage, no exceptions, no heap). Push/pop/peek/clear plus `drop_oldest()` and `push_overwrite()` helpers for the UART→TCP overflow policy.
- **`panel_model.h`** — abstract `PanelModel` base (`name()`, `zone_count()`, `area_count()`).
- **`panel_model_premier24.{h,cpp}`** — `PanelModelPremier24` placeholder returning `"Premier 24"`, 24 zones, 2 areas. Re-verify these in Phase 2 against the live panel.
- **`session_state.h`** — extracted `SessionState` class with `Monitor`/`Bridge` enum and pure `on_connect`/`on_disconnect`/`on_accept_while_busy` methods that return `Transition` enum values. Pure C++17, no ESPHome deps — exists solely so the host-side test can exercise transitions without AsyncServer.
- **`texecom.{h,cpp}`** — the `Texecom` ESPHome `Component`. AsyncTCP `AsyncServer` listens on the configured port; first client wins; second-client connection is rejected (`client->close(true)`) with a WARN log carrying the rejected IP. Two 1024-byte ring buffers (UART→TCP and TCP→UART) live as members. `loop()` cooperatively pumps in both directions.

### ESPHome YAML (`esphome/`)
- **`texecom-bridge.yaml`** — production config. `external_components` points at `../components`. UART block hard-codes the locked Plan 01-01 pinout: `tx_pin: GPIO5`, `rx_pin: GPIO6`, `baud_rate: 19200`, `data_bits: 8`, `parity: NONE`, **`stop_bits: 2`** (Wintex framing depends on the second stop bit). `texecom:` block wires `uart_id: panel_uart`, `tcp_port: 10001`. `esphome.libraries: ["AsyncTCP @ 1.1.1"]` pulls the TCP dep at compile time. Standard `wifi`/`api`/`ota`/`logger`/`captive_portal` sections using `!secret`.
- **`secrets.yaml.example`** — placeholder values for `wifi_ssid`, `wifi_password`, `ota_password`, `api_encryption_key` with generation instructions.

### Tests (`tests/`)
- **`CMakeLists.txt`** — CMake 3.20+, C++17, FetchContent-pulls Catch2 v3.5.2, registers tests via `catch_discover_tests`. Compiles the component headers + `panel_model_premier24.cpp` only — does NOT pull in any ESPHome framework headers.
- **`test_ring_buffer.cpp`** — 10 `TEST_CASE`s (~50 assertions): empty/full, round-trip, push-on-full fails, drain-on-empty fails, wrap-around, non-uint8 type instantiation (`int`), `size()` tracking, `peek` non-consumption, `push_overwrite` drops oldest, `clear()` reset.
- **`test_session_state.cpp`** — 9 `TEST_CASE`s covering: initial=Monitor, Monitor→Bridge, Bridge→Monitor, second-connect rejected with state preserved, `on_accept_while_busy` is non-mutating, disconnect-from-non-active is no-op, reconnect after disconnect, `reset()` returns to Monitor, reserved id 0 ignored.
- **`test_panel_model.cpp`** — 5 `TEST_CASE`s: Premier24 name/zones/areas accessors, polymorphism via `unique_ptr<PanelModel>`, in-test `FakeElite88` subclass proves the interface is extensible.

### Docs / housekeeping
- **`README.md`** — project paragraph, links to PROJECT.md/ROADMAP.md, primary `esphome run esphome/texecom-bridge.yaml` command, host-test invocation, project layout table.
- **`.gitignore`** — ignores `esphome/secrets.yaml`, `.esphome/`, `build/`, Python caches, editor junk.

## Public API surface (for Phase 3 to consume)

```cpp
namespace esphome::texecom {

class Texecom : public Component {
 public:
  void set_uart_parent(uart::UARTComponent *parent);
  void set_tcp_port(uint16_t port);
  void set_panel_model(PanelModel *model);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // Phase 3 will gate MQTT publishing on this:
  bool is_bridge_mode() const;

  PanelModel *panel_model() const;
  uint16_t tcp_port() const;
};

}  // namespace
```

`SessionState` is also exposed (in `session_state.h`) for Phase 2/3 code that wants to consult `mode()` directly without going through the component.

## Design decisions (worth re-reading before Plan 01-03)

- **Ring buffer sizes: 1024 bytes each direction.** Largest observed Wintex frame is ~512B; 1024 gives headroom for one frame in flight plus one queued without overflow, and fits comfortably in ESP32-S3 SRAM. Stored as `std::array` members (zero heap on the hot path).
- **Backpressure policy is asymmetric.**
  - `UART→TCP`: drop-oldest on overflow (Wintex re-sends on timeout; dropping a stale read is recoverable). Logged at WARN every 256 drops to avoid log spam.
  - `TCP→UART`: never drop (would corrupt panel config writes). The AsyncTCP `onData` callback only `ack()`s as many bytes as fit in the ring; the remainder triggers TCP back-pressure on the peer.
- **Single-client policy implemented via SessionState.** `Texecom::on_new_client_` calls `session_.on_connect(id)`; on `Rejected` it logs the rejected IP and immediately `client->close(true)`. The first session is untouched.
- **Session state is hardware-independent.** `SessionState` lives in its own header with no ESPHome includes so the host-side tests don't need to mock AsyncServer.
- **`AsyncTCP` over ESPHome sockets.** Mirrors the RoganDawes reference (closest prior art) and is well-tested on ESP32-S3 with the Arduino framework. Falling back to ESPHome's portable `socket` helpers is documented as a Plan-B if memory pressure shows up — none observed at scaffold time.
- **Default panel model.** If YAML doesn't supply one, `setup()` defaults to a static `PanelModelPremier24` so the component is always usable.

## Verification record

All file/grep verifications from the plan ran green:
- 11/11 file existence checks passed
- All 10 grep checks for required content passed (`stop_bits: 2`, `tx_pin: GPIO5`, `rx_pin: GPIO6`, `tcp_port: 10001`, `AsyncServer`, `is_bridge_mode` in both .h and .cpp, `class PanelModel`, `external_components`, `template`, Catch2)
- `texecom.cpp` is 261 lines (plan minimum 150)
- Test file counts: 10 ring-buffer cases, 9 session-state cases, 5 panel-model cases — all comfortably above the plan minima of 8/4/3.

### Host test build / ctest — NOT RUN

`cmake` is not on `PATH` in the project's MSYS bash environment, and neither `g++`/`gcc`/`cl` is available either. Tests have NOT been built or executed locally.

To run them, the user needs one of:
- **Visual Studio Build Tools 2022** (installs MSVC `cl.exe` + a CMake) — recommended on Windows.
- **MinGW-w64** + a standalone CMake install (e.g. via Scoop: `scoop install cmake gcc`).
- **WSL** with `sudo apt install build-essential cmake`.

After installing one of those, the documented invocation in `README.md` should work as-is:
```
cmake -S tests -B build/tests -Wno-dev
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

This is a host-toolchain gap, not a code defect. The tests are pure C++17 + Catch2 v3.5.2 (header-only on consumption; FetchContent compiles the static lib once) and have no platform-specific code paths.

### ESPHome compile / device upload — DEFERRED

`esphome compile` and `esphome upload` are explicitly Plan 01-03's job per the plan brief; running them requires the user's ESPHome Python env and a wired device. Not attempted here.

## Deviations from the plan

1. **`SessionState` lives in its own header (`session_state.h`)** rather than as a nested class inside `Texecom`. The plan implicitly required this (test file mandates `SessionState` extractability) and called it out in Task 3; just noting it explicitly.
2. **Added `push_overwrite()` and `drop_oldest()`** to `RingBuffer` so the UART→TCP overflow policy is expressed at the call site rather than re-implementing pop+push every time. Trivial extension; tested.
3. **`logger.logs.uart: WARN`** is set in the YAML to keep the panel UART out of the logger flood. Easy to comment out when debugging.
4. **AsyncTCP version pinned to `1.1.1`** in the YAML rather than left floating. Reduces "works yesterday, broken tomorrow" risk during the Plan 01-03 validation window.

None of these change the plan's contract or downstream interfaces.

## Known issues / risks for Plan 01-03 to validate

1. **Test suite has not been executed.** First action for the user: install a host C++ toolchain and run `ctest`. If anything fails there, fix it before booking on-device time.
2. **`AsyncTCP` write back-pressure path** (`pump_uart_to_tcp_` only stages what `client_->space()` reports) is logically correct but untested on-device — needs a long Wintex read-config to exercise.
3. **TCP→UART back-pressure** (the partial `client_->ack(i)` path in `on_client_data_`) has not been load-tested — large Wintex *write*-config is the canonical stressor.
4. **Second-client rejection path** is implemented but only host-tested at the SessionState level; on-device confirmation is a Plan 01-03 checklist item.
5. **`ESPAsyncTCP` vs `AsyncTCP`** — ESP32-S3 should pull `AsyncTCP` cleanly; if PlatformIO resolves the wrong fork (the Arduino-vs-ESPHome split) the SUMMARY recommends pinning, which we did.
6. **`framework: arduino`** is required for AsyncTCP. If Phase 4 wants to switch to esp-idf, the AsyncServer block in `texecom.cpp` is `#ifdef USE_ARDUINO`-gated and can be re-implemented against ESPHome's `socket` helpers without touching `SessionState`, `RingBuffer`, or `PanelModel`.

Plan 01-03's validation checklist should at minimum:
- Run `ctest` and confirm all suites green.
- `esphome compile esphome/texecom-bridge.yaml` (no upload yet) — confirm the AsyncTCP lib resolves.
- `esphome run esphome/texecom-bridge.yaml` to flash; check the boot log shows `TCP listener started on port 10001 (panel=Premier 24)`.
- Connect Wintex; read full config; verify the bridge log shows `Session Monitor -> Bridge`.
- Reject test: connect a second `nc` while Wintex is up; confirm WARN log + Wintex stays alive.
- Disconnect test: kill Wintex; confirm `Session Bridge -> Monitor` log + a follow-up Wintex re-connect succeeds.
