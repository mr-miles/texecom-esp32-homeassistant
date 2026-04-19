# Project State

## Current Position
- **Phase**: 1 in progress, Phase 2 planned
- **Status**: Phase 1 — code shipped + CI green; Plans 01-01 (bench) and 01-03 (Wintex validation) await user hardware time. Phase 2 — 3 plans across 3 sequential waves, second wave hardware-gated on user-supplied capture sessions.
- **Last Activity**: Phase 2 planning + ESP32 build fix (AsyncTCP fork) (2026-04-19)

## Progress
```
[██░░░░░░░░░░░░░░░░░░] 12% — 1.5/12 plans complete
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

## Hardware Setup Required Before Closing Phase 1 / Starting Phase 2
1. Wire Atom S3 ↔ Premier 24 per `.planning/hardware/phase-1-wiring.md`
2. Create `esphome/secrets.yaml` from `secrets.yaml.example`
3. Flash `esphome/texecom-bridge.yaml` via `esphome run esphome/texecom-bridge.yaml`
4. Install Wintex on a LAN-connected Windows box; configure UDL connection to `texecom-bridge.local:10001`
5. Run a Wintex session to close Plan 01-03 (Phase 1 done)
6. Build Phase 2 Plan 02-01 (adds capture infra), reflash, capture ≥3 Wintex sessions covering: cold-start full Receive, single-setting Send + verify, arm/disarm + tamper events
7. Drop captured `.bin` files under `tools/captures/` so Plan 02-02 can decode them

## Next Action
- **If hardware is ready**: continue `/legion:build` for Phase 1 Plan 01-03 (Wintex validation) → close Phase 1 → `/legion:build` for Phase 2 Plan 02-01 (capture infra)
- **If hardware is not ready**: Phase 2 plans are ready; Phase 1 code + Phase 2 plan files can be reviewed offline. Resume `/legion:build` whenever the bench is ready.
