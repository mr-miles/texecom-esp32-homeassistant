# Phase 2: Wintex Protocol Capture & Decode -- Context

## Phase Goal
Record Wintex↔panel traffic through the bridge, document the message grammar, and build a decoder that emits structured events for zones, areas, arm state, alarms, tamper, battery, mains, and siren — establishing the data layer that Phase 3 will publish to MQTT/HA.

## Requirements Covered
- **REQ-003 — Traffic logger**: Capture timestamped Wintex↔panel exchanges to on-device storage, downloadable for offline analysis
- **REQ-004 — Wintex protocol decoder**: Decode message types for zones, areas, arm/disarm, alarms, tamper, battery, mains, siren state
- **REQ-008 — Panel-type abstraction (populate)**: Fill in `PanelModelPremier24` with concrete message IDs and counts (Phase 1 shipped a placeholder with zone_count=24, area_count=2; this phase verifies and corrects those, plus populates the message-ID tables)

## What Already Exists (from prior phases)

### From Phase 1
- **`components/texecom/`**: Working ESPHome external component with TCP↔UART bridge on port 10001 (commit `15dcade`)
  - `texecom.h` / `texecom.cpp` — main component, AsyncTCP-backed; exposes `is_bridge_mode()` for Phase 3 to consume; will need `is_bridge_mode()`-gated capture taps in Phase 2
  - `session_state.h` — pure SessionState machine (Monitor/Bridge), host-testable
  - `ring_buffer.h` — header-only SPSC ring buffer template; can be reused for the capture buffer
  - `panel_model.h` — abstract `PanelModel` interface
  - `panel_model_premier24.{h,cpp}` — placeholder Premier 24 (name + zone_count=24 + area_count=2). Phase 2 will extend this with message-ID tables.
- **`esphome/texecom-bridge.yaml`**: Production config, GPIO5/6, 19200 8N2, port 10001, AsyncTCP@1.1.1
- **`tests/`**: Catch2 host-side test suite with FetchContent setup; pattern established for adding more tests
- **`.github/workflows/ci.yml`**: CI green — `host-tests` job runs `ctest`, `esphome-compile` job builds the YAML against dummy secrets
- **Hardware spec**: `.planning/hardware/phase-1-wiring.md` — Atom S3 GPIO5 TX / GPIO6 RX with 2k2/3k3 divider on the RX line

### Status of Phase 1 at Phase 2 start
- Plan 01-02 complete (code, YAML, tests)
- Plan 01-01 partial (spec written; on-bench Tasks 2-3 await user hardware time)
- Plan 01-03 pending (Wintex end-to-end validation, needs user with hardware + Wintex)

**Implication**: Phase 2 Plan 02-01 (capture infrastructure) can be built and unit-tested in parallel with the user's Phase 1 hardware bring-up. Phase 2 Plan 02-02 (grammar + decoder) is **blocked** until the user has captured ≥3 real Wintex sessions through the device — this can only happen after Phase 1 is on-device.

## Key Design Decisions

- **Three sequential waves**, second wave hardware-gated.
  Plan 02-01 produces firmware that records captures. Plan 02-02 needs the user to actually run that firmware on their panel and produce 3+ capture sessions before grammar work is meaningful — agents would otherwise hallucinate the protocol. Plan 02-03 closes the loop with regression tests.

- **Capture is on-device, not over-the-wire.**
  The bridge is already passing bytes between two parties; tapping bytes into a ring buffer + flushing to LittleFS is cheap and avoids needing the user to run pcap-style network captures. Downloads are exposed via ESPHome's `web_server` component (or a small HTTP route on a separate port) so the user can `curl` or browse to retrieve files.

- **LittleFS over SPIFFS.**
  SPIFFS is deprecated in modern Arduino-ESP32. LittleFS is the supported successor with better wear-levelling and crash safety. The Atom S3 has 8 MB flash — plenty of room for rotating capture files.

- **Capture files are paired binary + hex-dump.**
  Binary for byte-perfect replay into tests; hex-dump for visual inspection in a browser without downloading. Filename convention: `wintex-{unix_ts}.bin` + `wintex-{unix_ts}.txt`. Rotate at 256 KB or session-end (TCP client disconnect), whichever first.

- **Capture is gated to Bridge mode by default but user-overridable.**
  Phase 2's decoder runs in Monitor mode (no Wintex client connected). Capture is most useful in Bridge mode (recording real Wintex traffic). The capture writer hooks both pumps, but a YAML option `record_when: [bridge, monitor, both, none]` lets users record either or both. Default: `bridge` only.

- **Decoder is hardware-independent.**
  The `WintexDecoder` class consumes a byte stream (or a parsed framing layer) and emits structured events. No AsyncTCP, no UART — host-testable with golden-file fixtures. Same pattern as `SessionState` in Phase 1.

- **Plan 02-02 leans on prior community reverse-engineering.**
  The Wintex protocol has been partially reverse-engineered in the alarm community (DCM4 thread on automatedhome.co.uk, the RoganDawes README, scattered GitHub gists). The data-analytics-reporter agent should consult these before staring at hex dumps from scratch — combine prior art with user-supplied captures.

- **Premier 24 PanelModel verification.**
  The Phase 1 placeholder claimed `zone_count=24` and `area_count=2`. Plan 02-02 verifies these against the real panel via captured config-read responses and corrects if wrong (a Premier 24 may actually have 1 area in some configurations).

## Plan Structure
- **Plan 02-01 (Wave 1)**: On-Device Capture Infrastructure — `Capture` class, LittleFS persistence, HTTP download, host-side unit tests for the capture format
- **Plan 02-02 (Wave 2)**: Protocol Grammar + Decoder — Python capture-analysis tools, `.planning/protocol.md` grammar doc, `WintexDecoder` C++ class, populate `PanelModelPremier24` with real message IDs. **Hardware-gated**: requires user-supplied captures.
- **Plan 02-03 (Wave 3)**: Decoder Test Harness + Phase Close — golden-file regression suite, decoder wired into the texecom component for Monitor-mode emission, validation evidence, reality-checker gate
