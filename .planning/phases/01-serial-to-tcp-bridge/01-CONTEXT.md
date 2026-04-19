# Phase 1: Serial-to-TCP Bridge (Wintex-over-LAN) -- Context

## Phase Goal
An Atom S3 running ESPHome exposes the Premier 24's COM-header UART over TCP port 10001 so Wintex on Windows can connect over LAN and configure the panel as if directly plugged in.

## Requirements Covered
- **REQ-001 — Serial-to-TCP bridge**: ESP32 listens on TCP port 10001 and passes bytes transparently to/from the panel's COM header UART
- **REQ-002 — Wintex session transparency**: Wintex connecting to the bridge behaves identically to a local serial connection (handles handshake, packet framing, reconnects)
- **REQ-008 (initial skeleton)**: Code factored behind a `PanelModel` interface so switching panel models later is additive, not invasive. Phase 1 delivers the skeleton; Phase 2 populates the Premier 24 specifics.

## What Already Exists (from prior phases)
Nothing. This is the first executable phase. Prior work:
- `.planning/PROJECT.md` — project vision, constraints, key decisions
- `.planning/ROADMAP.md` — 5-phase plan, this is Phase 1 of 5
- `.planning/STATE.md` — current position tracker
- Empty repository aside from `.planning/`. No `src/`, no `platformio.ini`, no ESPHome YAML yet.

Reference projects to draw from (not committed here, but read before/during Plan 02):
- `https://github.com/RoganDawes/ESPHome_Wintex` — closest-fit existing ESPHome component
- `https://www.leocrawford.org.uk/2023/06/21/connecting-texecom-premier-using-esp8266.html` — wiring and pinout notes for a similar panel
- `https://universaldiscoverymethodology.com/2015/04/16/texecom-com-ip-controller-diy-and-cheap-too/` — early DIY COM-IP approach

## Key Design Decisions

- **Three sequential waves.** Hardware bring-up (Plan 01) must succeed before the ESPHome component is meaningful (Plan 02), which in turn must run before Wintex validation (Plan 03). No parallelism opportunity — each plan needs the previous plan's deliverable.
- **USB power only.** Level shifting and panel-powered operation are bundled into Phase 4. Plan 01 uses USB power and, if required, simple resistor-divider protection on the RX line from the panel's 5V TTL into the Atom S3's 3.3V input. If a direct connection works reliably, prefer that for simplicity in v1.
- **PanelModel skeleton in Phase 1, populated in Phase 2.** The interface exists so Plan 02 code compiles against a single model shape, but Premier 24 specifics (message IDs, zone counts) are deferred — in Phase 1 the model is a placeholder. This avoids premature abstraction while reserving the seam.
- **Single-client TCP server.** Wintex is the only expected client; supporting multiple simultaneous clients would force us to multiplex a physically exclusive resource (the panel's COM port). First connection wins; subsequent connection attempts are rejected until disconnect.
- **Host-side unit tests required.** The ring buffer, session state machine, and PanelModel abstraction are testable without hardware. These tests ship with the component and run in CI — per user preference captured in `PROJECT.md` constraints.
- **Rapid prototyper leads Plan 01.** Hardware bring-up is iterative and evidence-driven; rapid-prototyper's working style matches. Evidence-collector pairs in to capture scope traces as proof.
- **Senior developer leads Plan 02.** Implementation + tests are a single cohesive deliverable; backend-architect advises on buffer/state-machine design but doesn't need to write the code.
- **Reality-checker gates Plan 03.** Phase 1 is the foundation for every later phase. If the bridge is flaky, everything downstream is noise. Reality-checker's "NEEDS WORK" default is the right tool for a phase-close decision.

## Plan Structure
- **Plan 01-01 (Wave 1)**: Hardware Bring-Up & UART Sanity — wire Atom S3 to Premier 24 COM header, verify bidirectional bytes flow at 19200 baud, document the pinout.
- **Plan 01-02 (Wave 2)**: ESPHome External Component — TCP↔UART Bridge — scaffold the C++ component with `PanelModel` skeleton, implement TCP:10001 bridge, add host-side unit tests for the ring buffer and session state machine.
- **Plan 01-03 (Wave 3)**: Wintex End-to-End Validation — run Wintex over LAN, verify config read/write survives reboot, stress-test disconnect cycles, produce evidence document.
