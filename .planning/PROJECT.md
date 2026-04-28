# texecom-esp32-homeassistant

## What This Is
ESPHome firmware for an M5Stack Atom S3 that bridges a Texecom Premier 24 alarm panel to Home Assistant. Phase 1 exposes the panel's serial port over TCP/10001 so the Windows Wintex configuration tool can connect over LAN. Later phases reverse-engineer the Wintex protocol, publish panel state to MQTT with HA auto-discovery, and finally power the Atom S3 from the panel's 12V aux supply with proper level shifting.

## Core Value
Native Home Assistant integration for Texecom alarm panels using a ~£20 commodity ESP32 board instead of Texecom's proprietary COM-IP module — fully open, LAN-only, no cloud dependency.

## Who It's For
Primary: the project owner, for personal use with a Premier 24 installed at home. Secondary (Phase 5 onward): the Home Assistant / ESPHome community running Texecom Premier panels who want a reproducible open-source integration.

## Requirements

### Validated
- **REQ-010 — Bridge health telemetry** (closed 2026-04-28 with Phase 1.5): Three device-health entities (status, CPU temperature, WiFi signal) auto-discovered by HA via MQTT. End-to-end MQTT/discovery integration proven before Phase 3 layers panel data on top.

### Active
- **REQ-001 — Serial-to-TCP bridge**: ESP32 listens on TCP port 10001 and passes bytes transparently to/from the panel's COM header UART
- **REQ-002 — Wintex session transparency**: Wintex connecting to the bridge behaves identically to a local serial connection (handles handshake, packet framing, reconnects)
- **REQ-003 — Traffic logger**: Capture timestamped Wintex↔panel exchanges for offline protocol analysis
- **REQ-004 — Wintex protocol decoder**: Incrementally decode message types for zones, areas, arm/disarm, alarms, tamper, battery, mains, siren state
- **REQ-005 — MQTT + HA auto-discovery**: Publish decoded panel events to the existing Mosquitto broker using Home Assistant's MQTT discovery schema so entities appear automatically
- **REQ-006 — Panel power (hardware)**: Voltage regulator from panel 12V aux → 5V/3.3V feeding the Atom S3
- **REQ-007 — Level shifting (hardware)**: Proper bidirectional level shift between panel 5V TTL UART and Atom S3 3.3V logic
- **REQ-008 — Panel-type abstraction**: Code factored behind a panel-model interface so switching to other Premier Elite models (48/88/168/640) is additive, not invasive
- **REQ-009 — Community release**: README, wiring diagram, reproducible ESPHome YAML config, GitHub release tag
<!-- REQ-010 moved to Validated above on 2026-04-28 -->


### Out of Scope
- Keypad emulation / replacement
- Voice / speech module integration
- SMS, PSTN dialler, GSM dialler
- Any cloud / SaaS dependency
- Testing on panel models other than Premier 24 (architecture permits, but only Premier 24 will be shipped/tested in v1)
- Authentication / TLS on the TCP bridge (LAN-only, trusted network)
- MQTT publishing while a Wintex session is active (Wintex takes exclusive control)

## Constraints
- **Hardware (v1)**: M5Stack Atom S3 (ESP32-S3, 3.3V logic); Texecom Premier 24 panel with TTL-level UART on its internal RS232 header
- **Power (Phases 1-3)**: Atom S3 powered via USB; panel power integration deferred to Phase 4
- **Framework**: ESPHome (not bare ESP-IDF or Arduino) — custom Texecom protocol logic implemented as an ESPHome external C++ component
- **Network**: LAN-only deployment; no public internet exposure; unauthenticated TCP bridge is acceptable
- **MQTT broker**: Existing Mosquitto instance already serving Home Assistant
- **Build cadence**: Solo developer, guided execution mode, standard planning depth, balanced cost profile
- **Testing**: Unit tests required for all protocol/bridge/decoder/MQTT code — no code ships without test coverage for its non-trivial paths
- **Commit discipline**: Planning and code changes committed regularly as cohesive units of work

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| ESPHome over bare ESP-IDF | User deferred on framework; ESPHome gives native HA MQTT discovery, YAML config, OTA, and matches the RoganDawes reference project | Custom logic goes in an ESPHome external component |
| Premier 24 only, abstraction-ready | Only panel available for testing, but swapping models shouldn't require a rewrite | Panel-type abstraction is a Phase 2 architecture requirement |
| Wintex exclusivity over MQTT | Simpler state machine — MQTT pauses while Wintex is connected | No dual-use concurrency to design around |
| Unauthenticated LAN-only bridge | Same security posture as the panel's physical COM port; matches reference projects | No TLS / auth work in v1 |
| USB power during Phases 1-3 | Defers hardware complexity until firmware is proven | Level shifting + 12V regulator bundled into Phase 4 |
| MQTT bring-up split into Phase 1.5 (2026-04-28) | De-risks MQTT/HA discovery path early using built-in ESPHome sensors; doesn't need panel bench time | Phase 1.5 ships health telemetry; Phase 3 is now Wintex-data-only |
| Execution mode: Guided | User's chosen default — pauses at key decision points | Legion stops for confirmation between waves |
| Planning depth: Standard | User's chosen default | Balanced task breakdowns with explicit acceptance criteria |
| Cost profile: Balanced | User's chosen default | Haiku/Sonnet for routine work, Opus for complex reasoning and review |

## Architecture Influences
- **Reference implementations**: Leo Crawford's ESP8266 write-up (2023), Universal Discovery's DIY COM-IP (2015), and Rogan Dawes' `ESPHome_Wintex` GitHub project — the latter is closest to this architecture and will likely be the starting template for the ESPHome custom component.
- **Panel-type abstraction**: A `PanelModel` interface isolates Premier 24 specifics (message IDs, zone counts, area counts) from the transport/logger/MQTT layers. Adding a Premier Elite 48/88/168/640 later means implementing a second model class, not rewriting the core.
- **Wintex-exclusive state machine**: The firmware operates in two modes — *Bridge mode* (TCP client connected → raw passthrough, logging on, MQTT paused) and *Monitor mode* (no TCP client → decode + MQTT). A single exclusive-lock pattern avoids UART contention.
- **Hardware (Phase 4)**: 12V → 5V buck converter feeding Atom S3 USB 5V rail; bidirectional level shifter (e.g., TXB0104 or resistor-divider + diode-OR equivalent) between panel 5V TTL UART and Atom S3 3.3V GPIO. Component selection is an explicit Phase 4 task.
- **Logging strategy**: Structured timestamped packet captures (binary + human-readable dump) written to SPIFFS and exposed via ESPHome's web server for download. Supports offline analysis with Python/CyberChef/Wireshark.

---
*Last updated: 2026-04-19 after initialization*
