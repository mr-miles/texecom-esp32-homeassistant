# Phase 1 Wiring Specification — Atom S3 ↔ Texecom Premier 24 COM1

**Status:** forward spec (not a bench log). Wire to this document; evidence of
bring-up lives separately in `.planning/phases/01-serial-to-tcp-bridge/01-01-bringup-evidence.md`
(produced by Task 3 once hardware is on the bench).

This is the canonical wiring reference for Phases 1–3 of the project. Phase 4
will supersede it when a proper level shifter and panel-sourced power are
introduced.

---

## 1. Hardware

| Item | Value |
| --- | --- |
| MCU board | **M5Stack Atom S3** (ESP32-S3FN8, 3.3 V logic) — https://docs.m5stack.com/en/core/AtomS3 |
| Panel | **Texecom Premier 24** (panel revision: _TBD — user to record once the panel is in front of them_) |
| Cable | 4-core 7/0.2 mm stranded alarm cable OR jumper wires <200 mm for bench work. Keep the run short to minimise noise at 19200 baud. |
| Power (Phases 1–3) | USB-C from PC or 5 V/1 A wall wart into the Atom S3. **Do not** power the Atom from the panel during bring-up. |
| Protection parts | 1× 2.2 kΩ resistor, 1× 3.3 kΩ resistor (1/4 W, 5 % tolerance acceptable, 1 % preferred). |

---

## 2. Panel COM1 pinout

The Premier 24 main PCB carries a 5-pin header labelled **COM1** (the PC /
Wintex interface). Pinout below is taken from the public references cited at
the end of this document.

> **Verify against your panel's installation manual and PCB silkscreen before
> wiring.** Texecom has shipped minor silk/PCB variants; a wrong pin can put
> 12 V onto a GPIO. Meter every pin against panel GND before connecting the
> Atom.

| Pin | Function | Nominal voltage | Notes |
| --- | --- | --- | --- |
| 1 | +V (panel 12 V rail) | ~12 V DC | **Do NOT connect** in Phases 1–3. Reserved for Phase 4. |
| 2 | GND | 0 V | Common ground reference — must be tied to Atom GND. |
| 3 | Panel TX (to PC) | 5 V TTL, idle HIGH | Drives the Atom's RX via the divider below. |
| 4 | Panel RX (from PC) | 5 V TTL input, idle HIGH | Driven by the Atom's TX directly (3.3 V high reads as logic HIGH at a 5 V CMOS input). |
| 5 | +5 V (aux) | 5 V DC | **Do NOT connect** in Phases 1–3. |

---

## 3. Atom S3 GPIO assignments

Uses the **top 4-pin header** on the Atom S3 (the user-exposed pads are GPIO5,
GPIO6, GPIO7, GPIO8). GPIO5/6 are deliberately chosen: physically accessible,
clear of strapping pins (GPIO0/45/46), and clear of the onboard USB-CDC
console (GPIO19/20).

| Atom S3 pin | Function | Header | Notes |
| --- | --- | --- | --- |
| GPIO5 | UART TX → panel RX | Top 4-pin | 3.3 V push-pull. Direct to panel pin 4. |
| GPIO6 | UART RX ← panel TX | Top 4-pin | **Via resistor divider** (§5). Must not see 5 V. |
| GND  | Common ground | Bottom/back header | Tie to panel COM1 pin 2. |
| 5V in | USB-C only (Phases 1–3) | USB-C port | Panel 12 V/5 V stays disconnected. |

GPIO5 and GPIO6 are **locked** by Plan 01. Plan 02's ESPHome YAML hard-codes
these; any change here requires updating `esphome/texecom-bridge.yaml` in the
same commit.

---

## 4. Wiring diagram (text)

```
  Texecom Premier 24 COM1                       M5Stack Atom S3
  ------------------------                      --------------------
  Pin 1 (+12 V)  ─── NC (not connected in Phase 1-3)
  Pin 2 (GND)    ──────────────────────────────── GND (bottom header)
  Pin 3 (TX, 5V) ────┬── R1 = 2.2 kΩ ──┬────────── GPIO6 (RX)
                     │                 │
                     │             R2 = 3.3 kΩ
                     │                 │
                     │                 └────────── GND
                     (divider tap feeds GPIO6)
  Pin 4 (RX, 5V) ──────────────────────────────── GPIO5 (TX)   [direct, 3.3 V into 5 V input]
  Pin 5 (+5 V)   ─── NC (not connected in Phase 1-3)

  USB-C ─── PC or 5 V wall wart (Atom S3 power during Phases 1-3)
```

Keep the ground wire as short as the TX/RX wires, and twist/route them
together where possible to minimise skew at 19200 baud.

---

## 5. Resistor-divider math (panel TX → Atom RX)

We step 5 V TTL down to ~3.0 V, safely inside the ESP32-S3's 3.3 V GPIO limit
while still well above the V_IH threshold (~2.0 V).

```
V_out = V_in × R2 / (R1 + R2)
      = 5.0 V × 3.3 kΩ / (2.2 kΩ + 3.3 kΩ)
      = 5.0 V × 0.600
      = 3.00 V
```

- **R1 = 2.2 kΩ** (series, from panel TX to the divider tap)
- **R2 = 3.3 kΩ** (shunt, from the divider tap to GND)
- Wattage: 1/4 W is far more than needed (I ≈ 5 V / 5.5 kΩ ≈ 0.9 mA → 4.5 mW).
- Tolerance: 5 % is fine for bring-up; 1 % gives margin if the panel's drive
  voltage is on the high side.

This is a **Phase 1 bring-up hack**. A proper bidirectional level shifter
(TXB0104 or equivalent) is scheduled for Phase 4 and will replace this
divider, as well as adding proper protection on the Atom TX → panel RX
direction.

---

## 6. Baud rate and framing

**19200 baud, 8 data bits, no parity, 2 stop bits — 8N2.**

Source: RoganDawes `ESPHome_Wintex` reference (`example.yaml`) and the 2015
universaldiscoverymethodology.com write-up. The Wintex protocol framing
depends on the second stop bit; **do not** fall back to 8N1 — you will see
intermittent, hard-to-debug framing errors.

ESPHome UART config mirror:

```yaml
uart:
  tx_pin: GPIO5
  rx_pin: GPIO6
  baud_rate: 19200
  data_bits: 8
  parity: NONE
  stop_bits: 2
```

---

## 7. Power

- **Phases 1–3:** USB-C into the Atom S3 from either the developer's PC or a
  5 V/1 A wall wart. The panel's 12 V rail and 5 V aux are **left
  disconnected**. This keeps the Atom electrically isolated from any panel
  brown-out or surge during bring-up.
- **Phase 4 (deferred):** introduce a regulated 5 V take-off from the panel
  aux rail through a proper step-down + TXB0104 level shifter so the bridge
  runs on panel power when the panel is on battery. **Not in scope here.**

---

## 8. Known unknowns — verify before powering

Tick these off against the physical panel before applying power:

- [ ] COM1 header pin numbering matches this table on your specific Premier 24
      board revision (check the silkscreen and the installation manual).
- [ ] Panel TX idle voltage is ~5 V (not 12 V). Meter pin 3 to pin 2 (GND)
      with the panel powered and no Wintex client connected.
- [ ] Panel TX drive type — push-pull vs open-collector. A divider works for
      both, but an open-collector output biased with a pull-up may need a
      different R1/R2 ratio to hit 3.0 V.
- [ ] Panel revision (printed on the PCB near the main connector) — record in
      the Hardware table above.
- [ ] Atom S3 flashed and visible over USB-CDC **before** wiring to the panel.
- [ ] Continuity check: panel GND ↔ Atom GND is solid, with no other shared
      ground loops (e.g. PC chassis via USB + panel mains earth).

---

## 9. References

- RoganDawes, `ESPHome_Wintex` — https://github.com/RoganDawes/ESPHome_Wintex
  (ESP32 reference implementation; uses GPIO17 TX / GPIO16 RX on a generic
  dev board. We adapt to the Atom S3's exposed top-header pins.)
- Leo Crawford, "Connecting Texecom Premier using ESP8266" (2023) —
  https://www.leocrawford.org.uk/2023/06/21/connecting-texecom-premier-using-esp8266.html
- Universal Discovery Methodology, "Texecom COM-IP Controller — DIY and Cheap
  Too" (2015) —
  https://universaldiscoverymethodology.com/2015/04/16/texecom-com-ip-controller-diy-and-cheap-too/
  (original source of the 19200 8N2 framing spec).
- M5Stack Atom S3 docs — https://docs.m5stack.com/en/core/AtomS3

---

## 10. Change log

| Date | Author | Change |
| --- | --- | --- |
| 2026-04-19 | engineering-rapid-prototyper (Plan 01 Task 1) | Initial spec: GPIO5/6, 19200 8N2, 2k2/3k3 divider on panel TX. |
|  |  |  |
