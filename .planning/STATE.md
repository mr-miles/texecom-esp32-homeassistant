# Project State

## Current Position
- **Phase**: 1 in progress, Phase 1.5 executed (pending user hardware validation), Phase 2 planned
- **Status**: Phase 1 — code shipped + CI green; Plans 01-01 (bench) and 01-03 (Wintex validation) await user hardware time. Phase 1.5 — Plan 01.5-01 executed and committed; `esphome config` passes; phase-close awaits user running `01.5-VALIDATION.md` against the live broker + HA. Phase 2 — 3 plans across 3 sequential waves, second wave hardware-gated on user-supplied capture sessions.
- **Last Activity**: Phase 1.5 Plan 01.5-01 executed — MQTT block + 3 health sensors landed in `esphome/texecom-bridge.yaml` (2026-04-28)

## Progress
```
[███░░░░░░░░░░░░░░░░░] 19% — 2.5/13 plans complete
```

## Recent Decisions
- **Execution mode**: Guided — Legion pauses at key decision points during `/legion:build`
- **Planning depth**: Standard — balanced task breakdowns with explicit acceptance criteria
- **Cost profile**: Balanced — Haiku/Sonnet for routine tasks, Opus for complex reasoning and review
- **Framework**: ESPHome with custom external C++ component (over bare ESP-IDF)
- **Panel scope**: Premier 24 only in v1; architecture keeps other Premier Elite models additive
- **Testing**: Unit tests required for all protocol/decoder/bridge code; hardware phases use scope/multimeter evidence
- **Commit cadence**: Commit planning changes and code changes regularly (per user preference)
- **Phase 1 spec lock-in (2026-04-19)**: GPIO5 TX / GPIO6 RX, 19200 8N2, port 10001, AsyncTCP transport
- **Phase 1 build artefacts**: components/texecom/* + tests/* + esphome/texecom-bridge.yaml + CI green
- **AsyncTCP library (2026-04-19)**: Switched from upstream `AsyncTCP@1.1.1` to `esphome/AsyncTCP-esphome@^2.1.4` after ESP32 build failure (`xQueueHandle` was renamed to `QueueHandle_t` in modern ESP-IDF; ESPHome's fork tracks the toolchain)
- **Phase 2 decomposition**: 3 sequential waves — capture infrastructure → grammar+decoder (hardware-gated on user captures) → regression tests + phase close. Decoder is host-testable (no AsyncTCP dependency in its public API), gated to Monitor mode (Bridge mode pauses decoding per Wintex-exclusivity decision).
- **Phase 2 agents**: senior-developer + backend-architect (02-01); data-analytics-reporter + senior-developer (02-02); api-tester + evidence-collector + reality-checker (02-03)
- **Phase 1.5 inserted (2026-04-28)**: New MQTT bring-up slice between Phase 1 and Phase 2. Publishes 3 ESPHome built-in sensors (bridge status via LWT, CPU temperature, WiFi signal dBm) to existing Mosquitto with HA discovery. Independent of panel bench — can run concurrently with Phase 1 hardware bring-up. Plan files target `.planning/phases/01.5-mqtt-bringup/`. Adds REQ-010.
- **Phase 1.5 planning (2026-04-28)**: Single plan, single wave, single agent (engineering-senior-developer). 3 tasks: add `mqtt:` block + secrets entries; declare 3 sensors; validate config + write `01.5-VALIDATION.md` runbook. No C++ changes; pure ESPHome YAML using built-in `internal_temperature` + `wifi_signal` + a connectivity binary_sensor. No unit tests required (no non-trivial code path); validation runbook substitutes for automation since proof points need a real broker + HA.
- **Phase 1.5 executed (2026-04-28)**: Plan 01.5-01 complete. `esphome config esphome/texecom-bridge.yaml` exits 0. Files modified: `esphome/texecom-bridge.yaml`, `esphome/secrets.yaml.example`, plus new `01.5-VALIDATION.md` and `01.5-01-SUMMARY.md`. Side-effect: ESPHome auto-generated `esphome/.gitignore` (harmless, redundant with root gitignore). Phase-close awaits the user's manual validation pass against live Mosquitto + HA.

## Hardware Setup Required Before Closing Phase 1 / Starting Phase 2
1. Wire Atom S3 ↔ Premier 24 per `.planning/hardware/phase-1-wiring.md`
2. Create `esphome/secrets.yaml` from `secrets.yaml.example`
3. Flash `esphome/texecom-bridge.yaml` via `esphome run esphome/texecom-bridge.yaml`
4. Install Wintex on a LAN-connected Windows box; configure UDL connection to `<device_id>.local:10001` (the random 8-char id from the substitutions block at the top of `esphome/texecom-bridge.yaml`)
5. Run a Wintex session to close Plan 01-03 (Phase 1 done)
6. Build Phase 2 Plan 02-01 (adds capture infra), reflash, capture ≥3 Wintex sessions covering: cold-start full Receive, single-setting Send + verify, arm/disarm + tamper events
7. Drop captured `.bin` files under `tools/captures/` so Plan 02-02 can decode them

## Next Action
- **Phase 1.5 close (user, no agent)**: Update `esphome/secrets.yaml` with three new keys (`mqtt_broker_host`, `mqtt_broker_username`, `mqtt_broker_password`) pointing at the existing Mosquitto. Then follow `.planning/phases/01.5-mqtt-bringup/01.5-VALIDATION.md` step by step: flash → confirm 3 entities in HA → LWT test → broker-restart test → WiFi-blip test. Tick the phase-close checklist; that's the artefact that closes Phase 1.5.
- **Optional**: `/legion:review` to run a review pass over the Phase 1.5 code+plan deliverables before flashing.
- **If panel hardware is ready**: `/legion:build` for Phase 1 Plan 01-03 (Wintex validation) → close Phase 1 → `/legion:build` for Phase 2 Plan 02-01 (capture infra).
- **If panel hardware is not ready**: Phase 2 plans are written. Resume `/legion:build` whenever the bench is ready.
