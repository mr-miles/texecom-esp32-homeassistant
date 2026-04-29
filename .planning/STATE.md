# Project State

## Current Position
- **Phase**: 1 in progress, **Phase 1.5 closed**, Phase 2 planned
- **Status**: Phase 1 — code shipped + CI green; Plans 01-01 (bench) and 01-03 (Wintex validation) await user hardware time. Phase 1.5 — closed 2026-04-28; 8/9 phase-close criteria ticked against real hardware (3 entities live in HA, LWT works, Mosquitto-restart recovery works); WiFi-blip test skipped (no easy router-side MAC-block at site) but the reconnect path is exercised by the LWT + broker-restart paths. Phase 2 — 3 plans across 3 sequential waves, second wave hardware-gated on user-supplied capture sessions.
- **Last Activity**: Phase 1.5 closed — first end-to-end Texecom Bridge → Mosquitto → HA discovery success (2026-04-28)

## Progress
```
[████░░░░░░░░░░░░░░░░] 27% — 3.5/13 plans complete
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
- **Phase 1.5 cleanup (2026-04-28)**: Stripped `api:` and `web_server:` from default install (HA integration via MQTT-only; web_server re-enables when Phase 2 needs capture downloads). Added `device_id` substitution (random 8-char hex) baked into mDNS hostname `texecom-bridge-${device_id}.local` and MQTT topic subtree to prevent multi-bridge collisions on shared LAN/broker. Added `mqtt_port` and `mqtt_base_topic` substitutions for tunable defaults.
- **Phase 1.5 closed (2026-04-28)**: First end-to-end Texecom Bridge → Mosquitto → HA discovery success. 3 entities visible in HA (status/cpu_temp/wifi_signal). LWT verified (offline-on-disconnect within 60s). Broker-restart recovery verified. WiFi-blip recovery test skipped (no easy router-side MAC-block at site); coverage judged sufficient via the other reconnect-path tests. Validation runbook ticks recorded in 01.5-VALIDATION.md.
- **Lesson learned (2026-04-28)**: Bring-up burned ~30 min because the chip was running stale unrelated firmware (F1p Ecodan code from a comparison experiment) and OTA upload couldn't reach it. VS Code ESPHome extension only offers compile + OTA, so initial recovery flashing required `esphome run --device COM3` from the CLI. Captured to memory.
- **Phase 2 priority — Plan 02-01 fix-up first (2026-04-28)**: Plan 02-01 shipped the capture writer to LittleFS but never wired the HTTP download route the SUMMARY claimed. Before any other Phase 2 work, add a `/captures/` handler to `components/texecom/` that hooks into ESPHome's WebServerBase to provide directory listing + file streaming for the on-device `.bin` + `.txt` capture files. ~30-50 LOC C++. User wants this as the first thing in Phase 2 so the download UX exists before Plan 02-02's grammar/decoder work needs real captures.
- **Plan 02-01 fix-up shipped (2026-04-29)**: New `components/texecom/capture_http.{h,cpp}` (~370 LOC including HTML listing + path validator + chunked streaming via `httpd_resp_send_chunk`). Path safety via host-testable `is_safe_capture_filename()` with extension whitelist (`.bin`/`.txt`); 6 Catch2 cases in `tests/test_capture_http_paths.cpp`. `esphome compile` clean. Live verification (HTML listing, download prompts, traversal-blocked-over-the-wire) still pending OTA + a real Wintex capture session at the bench. Plans 02-02 + 02-03 remain hardware-gated.

## Hardware Setup Required Before Closing Phase 1 / Starting Phase 2
1. Wire Atom S3 ↔ Premier 24 per `.planning/hardware/phase-1-wiring.md`
2. Create `esphome/secrets.yaml` from `secrets.yaml.example`
3. Flash `esphome/texecom-bridge.yaml` via `esphome run esphome/texecom-bridge.yaml`
4. Install Wintex on a LAN-connected Windows box; configure UDL connection to `texecom-bridge-<device_id>.local:10001` (the random 8-char id from the substitutions block at the top of `esphome/texecom-bridge.yaml`)
5. Run a Wintex session to close Plan 01-03 (Phase 1 done)
6. Build Phase 2 Plan 02-01 (adds capture infra), reflash, capture ≥3 Wintex sessions covering: cold-start full Receive, single-setting Send + verify, arm/disarm + tamper events
7. Drop captured `.bin` files under `tools/captures/` so Plan 02-02 can decode them

## Next Action
- **Immediate**: OTA-flash the Plan 02-01 fix-up firmware (commit 55fce6b). Browse to `http://texecom-bridge-2f7dc4b9.local/captures/` — listing page should render. There won't be any files until the panel is wired and a Wintex session runs, but the empty listing + 404-on-traversal would already prove the route is alive.
- **Phase 1 close still pending hardware**: Plan 01-03 (Wintex validation) needs the bench wired up — see "Hardware Setup" below. Closing Plan 01-03 closes Phase 1.
- **If panel hardware is ready**: `/legion:build` for Phase 1 Plan 01-03 (Wintex validation) → close Phase 1 → run a Wintex session to populate `/captures/` → download captures via the new web UI → `/legion:build` for Phase 2 Plan 02-02 (decoder, host-testable against the captures).
- **If panel hardware is not ready**: Plans 02-02 + 02-03 stay queued. Code is fine to sit; CI is green.
