# texecom-esp32-homeassistant

ESPHome firmware for an M5Stack Atom S3 that bridges a Texecom Premier 24 alarm panel to Home Assistant. Phase 1 exposes the panel's serial port over TCP/10001 so the Windows Wintex configuration tool can connect over LAN; later phases reverse-engineer the Wintex protocol, publish panel state to MQTT with HA auto-discovery, and finally power the Atom S3 from the panel's 12V aux supply with proper level shifting.

See [`.planning/PROJECT.md`](.planning/PROJECT.md) for vision, requirements, and constraints, and [`.planning/ROADMAP.md`](.planning/ROADMAP.md) for the phase-by-phase plan.

## Build and flash the Atom S3

Primary command (from the repo root):

```
esphome run esphome/texecom-bridge.yaml
```

First-time setup: copy `esphome/secrets.yaml.example` to `esphome/secrets.yaml` and fill in your Wi-Fi, OTA, and MQTT broker secrets. The real `secrets.yaml` is `.gitignore`'d.

Wiring (locked in Plan 01-01): GPIO5 = TX (Atom S3 -> panel), GPIO6 = RX (panel -> Atom S3), 19200 8N2. See `.planning/hardware/phase-1-wiring.md` for the full pinout and protection-resistor decisions.

Per-device id: `esphome/texecom-bridge.yaml` carries a random 8-char `device_id` substitution at the top. It's used as the mDNS hostname, the HA friendly-name suffix, and the MQTT topic subtree, so two bridges on the same LAN/broker don't collide. Regenerate before flashing a second device — the comment above the substitution shows the one-line Python command.

Once flashed, point Wintex (or any TCP client) at `<device_id>.local:10001` (the value from your YAML). Only one client is allowed at a time; a second connection is rejected cleanly while the first session is preserved.

## Run the host-side test suite

The ring buffer, session state machine, and `PanelModel` interface have host-side unit tests using Catch2 v3 (fetched automatically by CMake). Requires CMake 3.20+ and a C++17 compiler (gcc, clang, or MSVC).

```
cmake -S tests -B build/tests -Wno-dev
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

On Windows you'll need either Visual Studio Build Tools (MSVC) or MinGW-w64 on `PATH`. WSL also works.

## Project layout

| Path | Purpose |
| --- | --- |
| `components/texecom/` | ESPHome external component (TCP<->UART bridge, panel-model abstraction) |
| `esphome/texecom-bridge.yaml` | Production ESPHome config — flash this to the Atom S3 |
| `esphome/bringup-uart-logger.yaml` | Plan 01-01 minimal UART logger config |
| `tests/` | Host-side Catch2 unit tests |
| `.planning/` | Project vision, roadmap, phase plans, and per-plan summaries |

## Status

Phase 1 (Serial-to-TCP Bridge) — in progress. See `.planning/ROADMAP.md` for the live progress table.
