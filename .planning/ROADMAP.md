# texecom-esp32-homeassistant — Roadmap

## Phases

- [ ] **Phase 1** — Serial-to-TCP Bridge (Wintex-over-LAN)
- [ ] **Phase 2** — Wintex Protocol Capture & Decode
- [ ] **Phase 3** — MQTT + Home Assistant Auto-Discovery
- [ ] **Phase 4** — Panel-Powered Hardware
- [ ] **Phase 5** — Community Release Polish

## Phase Details

### Phase 1: Serial-to-TCP Bridge (Wintex-over-LAN)
**Goal**: An Atom S3 running ESPHome exposes the Premier 24's COM header UART over TCP port 10001 so Wintex on Windows can connect over LAN and configure the panel as if directly plugged in.

**Requirements**: REQ-001, REQ-002, REQ-008 (initial abstraction skeleton)

**Recommended Agents**:
- `engineering-backend-architect` — ESPHome component architecture, UART↔TCP buffering strategy, panel-model interface design
- `engineering-senior-developer` — C++ implementation of the ESPHome external component
- `engineering-rapid-prototyper` — Hardware bring-up on Atom S3, UART wiring iteration with scope
- `testing-api-tester` — Validates bridge byte-integrity and Wintex session stability
- `project-manager-senior` — Wave/task breakdown, acceptance-criteria gating

**Success Criteria**:
- Wintex running on a LAN-connected Windows box connects to `esp32.local:10001` and reads the panel's configuration without error
- Wintex can write a trivial config change (e.g. site name) and verify the write survives a reboot
- No dropped bytes at 19200 baud over a 60s large-payload session
- TCP session recovers cleanly after a client disconnect and reconnect
- `PanelModel` interface exists (even if trivially populated) so Phase 2 has a hook for Premier 24 specifics
- Unit tests cover the UART↔TCP buffering layer and any non-trivial helper code; test suite runs in CI against the ESPHome component

**Plans**: 3 (hardware bring-up, ESPHome component scaffolding, Wintex validation)

---

### Phase 2: Wintex Protocol Capture & Decode
**Goal**: Record Wintex↔panel traffic through the bridge, document the message grammar, and build a decoder that emits structured events for zones, areas, arm state, alarms, tamper, battery, mains, and siren.

**Requirements**: REQ-003, REQ-004, REQ-008 (fill out Premier 24 model)

**Recommended Agents**:
- `engineering-senior-developer` — Protocol decoder in the ESPHome component; state-machine design
- `engineering-rapid-prototyper` — Packet-analysis tooling, Python scripts for offline decoding of captures
- `data-analytics-reporter` — Structured parsing of capture logs, grammar extraction
- `testing-api-tester` — Regression tests against recorded captures; golden-file comparisons
- `testing-evidence-collector` — Captures Wintex UI screenshots paired with decoded packet dumps as proof of mapping

**Success Criteria**:
- Documented message grammar (markdown in `.planning/protocol.md`) covering at minimum: heartbeat, zone state, area state, arm/disarm command+ack, alarm/tamper events, battery/mains status
- Capture tool writes timestamped binary + hex-dump logs to on-device storage, downloadable via ESPHome's web server
- Decoder unit tests pass against at least 3 recorded Wintex sessions
- Decoder surfaces at least 8 distinct event types as structured objects
- `PanelModel` for Premier 24 populated with correct zone/area counts and message IDs

**Plans**: 3 (capture infrastructure, grammar documentation + decoder, test harness)

---

### Phase 3: MQTT + Home Assistant Auto-Discovery
**Goal**: Decoded panel events publish to the existing Mosquitto broker using Home Assistant's MQTT discovery schema. HA automatically creates entities for zones, areas, arm state, alarms, tamper, battery, and mains — no manual YAML in HA.

**Requirements**: REQ-005

**Recommended Agents**:
- `engineering-backend-architect` — MQTT topic taxonomy, HA discovery payload schema, retention/QoS strategy
- `engineering-senior-developer` — Implementation inside the ESPHome component
- `testing-api-tester` — MQTT message validation, discovery config correctness
- `testing-evidence-collector` — Home Assistant UI screenshots showing auto-discovered entities and live state
- `testing-reality-checker` — Production-readiness gate before phase close

**Success Criteria**:
- All zones, areas, arm state, alarm, tamper, battery, mains, and siren entities auto-appear in HA within 30s of device boot
- State updates propagate from a physical panel event to the HA entity in <2s
- Device recovers cleanly from a Mosquitto broker restart (entities remain, state re-publishes)
- Wintex-mode exclusivity holds: MQTT pauses cleanly when a Wintex client connects and resumes on disconnect
- HA long-term statistics begin capturing the appropriate sensors (battery voltage, etc.)
- Unit tests cover MQTT payload construction, HA discovery schema generation, and event→topic mapping

**Plans**: 2 (topic/discovery design + implementation, HA integration validation)

---

### Phase 4: Panel-Powered Hardware
**Goal**: Retire the USB cable. The Atom S3 is powered from the Premier 24's 12V aux supply through a voltage regulator, and the UART connection is properly level-shifted between the panel's 5V TTL and the Atom S3's 3.3V logic.

**Requirements**: REQ-006, REQ-007

**Recommended Agents**:
- `engineering-rapid-prototyper` — Buck-converter selection, level-shifter prototyping, breadboard → stripboard iteration
- `testing-evidence-collector` — Scope traces, multimeter readings, thermal imaging as documented proof of hardware behaviour
- `testing-performance-benchmarker` — 72h soak test, UART-burst stability, thermal load characterisation
- `product-technical-writer` — Wiring diagram, parts list, BOM, soldering guide

**Success Criteria**:
- Device runs ≥72h continuous on panel power alone without USB
- Panel's 12V rail stays within spec under UART-burst load (verified on scope)
- Level shifter handles sustained 19200 baud in both directions with no waveform degradation
- Thermal readings on the regulator stay within manufacturer spec under worst-case load
- Wiring diagram, parts list (with specific part numbers + suppliers), and assembly notes committed to the repo

**Plans**: 2 (hardware prototype + validation, documentation)

---

### Phase 5: Community Release Polish
**Goal**: A Home Assistant or ESPHome user who finds this repo can replicate the setup end-to-end from the README, with a tagged GitHub release to install.

**Requirements**: REQ-009

**Recommended Agents**:
- `product-technical-writer` — README, user guide, wiring guide, troubleshooting
- `engineering-senior-developer` — Cleanup pass, reusable ESPHome YAML config, example substitutions
- `marketing-content-creator` — Launch announcement (HA community forum post, ESPHome discord)
- `testing-reality-checker` — Final production-readiness certification before v1.0 tag

**Success Criteria**:
- README covers: hardware list, wiring, ESPHome YAML config, HA setup, troubleshooting — each section with photos/diagrams
- GitHub release v1.0 tagged with compiled firmware binary as an asset
- One external user (recruited via HA forum or known Texecom community) successfully replicates the build and reports back
- Repository has issue templates, CONTRIBUTING.md, and an LGPL/MIT-compatible LICENSE
- Project listed / linked from relevant community indices (ESPHome external components awesome-list, HA community forum pinned thread)

**Plans**: 2 (documentation + release, external replication validation)

## Progress

| Phase | Plans | Completed | Status |
|-------|-------|-----------|--------|
| 1. Serial-to-TCP Bridge | 3 | 1.5 | In progress (01-01 partial, 01-02 done, 01-03 pending hardware) |
| 2. Wintex Protocol Capture & Decode | 3 | 0 | Not started |
| 3. MQTT + HA Auto-Discovery | 2 | 0 | Not started |
| 4. Panel-Powered Hardware | 2 | 0 | Not started |
| 5. Community Release Polish | 2 | 0 | Not started |
| **Total** | **12** | **0** | — |
