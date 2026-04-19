# Plan 01-01 — Hardware Bring-Up & UART Sanity (PARTIAL)

## Status
**PARTIAL** — Task 1 complete; Tasks 2 and 3 deferred until hardware is on the bench.

## What Was Done (Task 1)
Wrote `.planning/hardware/phase-1-wiring.md` as a forward spec — the user wires *to* this document rather than discovering pins during bring-up.

**Locked decisions** (carried into Plan 01-02):
- TX pin: GPIO5 (Atom S3 → panel)
- RX pin: GPIO6 (panel → Atom S3)
- Serial: 19200 8N2 (8 data bits, no parity, **2 stop bits** — confirmed against RoganDawes/ESPHome_Wintex `example.yaml` and the universaldiscoverymethodology.com 2015 reference; Wintex framing depends on the second stop bit)
- Protection: 2k2 / 3k3 resistor divider on panel TX → Atom RX (5V → 3.0V exact); direct connection on Atom TX → panel RX
- Power: USB-C only during Phases 1-3; panel-powered operation deferred to Phase 4
- TXB0104 / proper level shifter: deferred to Phase 4

## What Was NOT Done (Tasks 2 & 3)
- **Task 2** — `esphome/bringup-uart-logger.yaml` flashing test: NOT executed. Plan 01-02 went directly to the production `esphome/texecom-bridge.yaml` instead, since the pin/baud spec is already locked. The bring-up YAML can still be created if the user wants an isolated UART-logger sanity check before flashing the full component.
- **Task 3** — Bench evidence (Wi-Fi online, panel bytes in log, scope trace of UART timing): NOT executed. Requires user with Atom S3, panel access, and a scope/logic analyser. Queued for hardware time.

## Red Flags for the User to Verify Before Wiring
1. **COM1 pin numbering** can vary between Premier 24 board revisions — verify silkscreen and the installation manual before applying power.
2. **Pin voltage**: meter panel pin 3 → pin 2 with the panel powered to confirm ~5V (not 12V) before connecting to the divider.
3. **TX drive type**: check whether panel TX is push-pull or open-collector. An open-collector output biased with an internal pull-up may shift the divider ratio and need R1/R2 adjusted.
4. **Premier 24 PCB revision**: record in the wiring doc's Hardware table (TBD placeholder).
5. **Ground loops**: ensure no ground-loop between PC USB chassis ground and panel mains earth during bench testing.

## Inputs to Plan 01-02
GPIO5 / GPIO6 / 19200 8N2 / port 10001 / AsyncTCP transport — all lifted into the production YAML and component code by Plan 01-02.

## Inputs to Plan 01-03
Tasks 2 and 3 of this plan (bench bring-up with `bringup-uart-logger.yaml`, scope trace, panel-bytes-in-log evidence) effectively merge with Plan 01-03's on-device validation. Plan 01-03 should:
1. Optionally create + flash `bringup-uart-logger.yaml` first as a sanity check OR skip straight to flashing `esphome/texecom-bridge.yaml`.
2. Capture scope trace evidence as part of its Test 1 (Wintex read/write) or as a pre-test sanity check.

## Verdict
PARTIAL pass — spec locked, code-side dependencies unblocked. Hardware-side close-out folded into Plan 01-03.
