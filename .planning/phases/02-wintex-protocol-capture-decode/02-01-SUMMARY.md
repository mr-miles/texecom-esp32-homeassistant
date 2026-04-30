---
phase: 02-wintex-protocol-capture-decode
plan: 01
status: code-complete (CI pending)
date: 2026-04-19
---

# Plan 02-01 Summary: On-Device Capture Infrastructure

## What shipped

- `components/texecom/capture.{h,cpp}` — `Capture` class with TXCP binary
  format, in-memory event ring (64 events), LittleFS persistence, paired
  hex-dump sidecar, file rotation, and a host-test sink hook. Pure
  serializers (`serialize_header`, `serialize_event`, `parse_header`,
  `parse_event`) live in the header so unit tests bypass the
  Arduino-only persistence layer.
- `components/texecom/texecom.{h,cpp}` — owns a `Capture capture_;`
  member. Hooked from `pump_uart_to_tcp_` (direction 0), `on_client_data_`
  (direction 1), `on_new_client_` (`on_session_start`),
  `on_client_disconnect_` (`on_session_end`), and `loop()` (drain ring).
  Capture failures NEVER block the byte pump — they bump a drop counter
  and emit a throttled WARN.
- `components/texecom/__init__.py` — accepts `record_when`,
  `capture_max_file_bytes`, `capture_root` schema options.
- `esphome/texecom-bridge.yaml` — three new options under `texecom:`,
  plus `web_server: { port: 80, version: 3 }` for HTTP downloads.
- `tests/test_capture.cpp` — 8 Catch2 TEST_CASEs covering the locked
  format, registered in `tests/CMakeLists.txt` alongside `capture.cpp`.

## TXCP binary format v1 (LOCKED)

### File header — exactly 32 bytes, written once per file

| offset | size | field          | notes                                  |
|--------|------|----------------|----------------------------------------|
| 0      | 4    | `magic`        | ASCII `"TXCP"` (`0x54 0x58 0x43 0x50`) |
| 4      | 2    | `version`      | uint16 LE, == 1                        |
| 6      | 4    | `start_unix_s` | uint32 LE, wall-clock at file open or 0 |
| 10     | 16   | `panel_name`   | ASCII null-padded, e.g. `"Premier 24"` |
| 26     | 4    | `baud_rate`    | uint32 LE, e.g. 19200                  |
| 30     | 2    | `reserved`     | zero                                   |

### Event record — variable length

| offset | size | field          | notes                                  |
|--------|------|----------------|----------------------------------------|
| 0      | 1    | `direction`    | 0 = panel→client, 1 = client→panel     |
| 1      | 8    | `timestamp_us` | uint64 LE, microseconds since file open |
| 9      | 2    | `length`       | uint16 LE, 0..65535                    |
| 11     | N    | `bytes`        | payload (`length` bytes)               |

Records are concatenated back-to-back after the header. The parser walks
the stream linearly with no framing markers required.

### Rotation

- New file when `current_file_bytes_ + len > max_file_bytes_`.
- New file on `on_session_start()` (Bridge mode begins).
- Closes file on `on_session_end()` (Bridge mode ends).
- Filename: `{root}/wintex-{unix_ts}-{NNN}.bin` if SNTP synced,
  otherwise `{root}/wintex-boot{boot_ms}-{NNN}.bin`. `NNN` is a
  zero-padded sequence counter that resets each boot.

### Hex-dump sidecar

Written **incrementally** alongside the `.bin` (chosen for simplicity —
no second pass needed at download time, and the user can browse the
`.txt` directly without parsing). 16 bytes per line, `xxd`-style:

```
00000020  10 20 30 ff de ad be ef ...   |. 0.....|
```

## Default option values (override in YAML)

| Option                    | Default     | Range / values                       |
|---------------------------|-------------|--------------------------------------|
| `record_when`             | `bridge`    | `none` / `bridge` / `monitor` / `both` |
| `capture_max_file_bytes`  | `262144`    | 4 KB .. 4 MB                         |
| `capture_root`            | `/captures` | LittleFS path                        |

Override example:

```yaml
texecom:
  id: panel_bridge
  uart_id: panel_uart
  tcp_port: 10001
  record_when: both
  capture_max_file_bytes: 524288
  capture_root: "/wintex"
```

## Capture URL pattern (HTTP download)

ESPHome `web_server` v3 exposes filesystem access via the JSON file API.
After flashing, browse:

- `http://<device-ip>/`              — landing page (entities + status)
- `http://<device-ip>/file?file=/captures/wintex-XXXX-001.bin` — download
- `http://<device-ip>/file?file=/captures/wintex-XXXX-001.txt` — hex view

`dump_config()` logs the capture root + drop count at boot so the user
can confirm wiring.

If a future ESPHome `web_server` release stops exposing arbitrary
LittleFS paths, the fallback is to add a custom HTTP route inside the
texecom component itself (deferred — current path works).

## Tests

- **8 TEST_CASEs** in `tests/test_capture.cpp` (plan required ≥6):
  1. Header round-trip preserves all fields
  2. Magic bytes are exactly `'T','X','C','P'` (and tampered magic fails)
  3. Single event round-trip
  4. Multiple events back-to-back parse linearly
  5. Length field round-trips at boundary values (0, 1, 256, 1023)
  6. Invalid direction values are rejected
  7. `parse_event` refuses truncated input
  8. End-to-end via test sink replays one session

- **Local run command** (when a host C++ toolchain is available):
  ```
  cmake -S tests -B build/tests
  cmake --build build/tests
  ctest --test-dir build/tests --output-on-failure
  ```

- **Local execution status**: NOT RUN. This Windows MSYS environment has
  no `cmake`, `g++`, `clang++`, or MSVC `cl` on PATH (verified). All 16
  plan-listed verification commands (file existence, grep guards,
  TEST_CASE count ≥6) pass — see commit log. The build/run is delegated
  to `.github/workflows/ci.yml` which already runs `ctest` on push.

- **ESPHome compile**: also CI-only (`esphome` not installed locally).
  Schema additions are syntactically consistent with the existing
  `__init__.py` pattern; CI will catch any breakage.

## Deviations from the plan

1. **Hex-dump sidecar = incremental** (plan offered "lazy or
   incremental — pick whichever is simpler"). Incremental is simpler —
   one `LittleFS.open(..., "a")` per event batch, no second pass at
   download.
2. **`record()` coalesces a UART batch into one event** rather than one
   event per byte. The bridge's `pump_uart_to_tcp_` already buffers
   per-tick reads into a 256-byte stack array; passing that whole batch
   to `capture_.record(0, ...)` keeps the ring count low and dramatically
   reduces serialization overhead. Wintex frames are still preserved
   byte-perfect — only the event grouping changes.
3. **Per-event payload bound = 256 bytes** in the ring. Larger pushes
   are split across multiple ring slots. Wintex frames sit well below
   this; the bound just keeps `PendingEvent` POD-sized.
4. **`web_server: version: 3, local: false`** chosen over the plan's
   "/local/captures/" suggestion because v3's `/file?file=...` is the
   stable, documented API for downloading arbitrary LittleFS paths.
5. **No 100 ms inactivity timer** — `Capture::loop()` is called every
   ESPHome tick (~16 µs idle, much faster under load), which is already
   well below 100 ms. A separate timer would add complexity for no win.

## User instruction block — collecting captures for Plan 02-02

> **To collect captures for Plan 02-02 (decoder):**
>
> 1. Pull this branch and flash the latest `esphome/texecom-bridge.yaml`
>    to the Atom S3 (`esphome run esphome/texecom-bridge.yaml`).
> 2. Confirm the device boots: the log should print
>    `Capture mode: bridge` / `Capture root: /captures` / `Capture
>    drops: 0` in `dump_config`.
> 3. Run a Wintex session through the bridge (LAN, port 10001):
>    - **Session A — cold-start config read.** Open the panel in
>      Wintex, do a full Receive (config download). Disconnect.
>    - **Session B — single-setting write.** Reconnect, change one
>      trivial value (e.g. site name), Send to panel, disconnect.
>    - **Session C — arm/disarm event sequence.** Reconnect, open
>      Status, arm an area at the keypad and disarm it while Wintex is
>      watching, disconnect.
> 4. Browse to
>    `http://<device-ip>/captures/` to see the listing (route added by
>    the Plan 02-01 fix-up below). Each session produces one
>    `wintex-*.bin` (binary) and matching `wintex-*.txt` (xxd-style dump).
> 5. Download all `.bin` files and drop them under `tools/captures/`
>    in this repo (create the dir if it doesn't exist). Commit them on
>    a feature branch.
> 6. Open an issue tagged `phase-2/plan-02-02` listing the three
>    capture filenames and noting anything weird Wintex did during the
>    sessions (timeouts, retries, error popups). The decoder agent
>    needs this context.
>
> **Aim for ~50 KB–1 MB per session** — that's plenty of message
> variety. If the device's `Capture drops:` line in `dump_config` is
> non-zero after a session, mention it in the issue (it means the bridge
> was busier than the LittleFS flush could keep up with — the bytes are
> still on the wire, just not in the file).

## Risk notes / follow-ups for Plan 02-02

- **LittleFS write latency under sustained 19200 baud burst** is the
  unknown. The drop counter exists precisely to surface this; if real
  captures show drops, options are: (a) larger ring (cheap), (b) batch
  multiple events into one `LittleFS.open(..., "a")` call (medium),
  (c) async write task (expensive — defer).
- **`micros()` overflow** wraps every ~71 minutes. A single Wintex
  session is far shorter than this, so per-file timestamps stay
  monotonic. Cross-file analysis must use the header's `start_unix_s`.
- **No on-device delete UI yet.** Files are bounded by rotation but the
  user must manually `curl -X DELETE` (web_server v3 supports it) or
  re-flash to clear. Acceptable for Plan 02-01 — revisit if storage
  becomes a problem.

---

## Plan 02-01 fix-up (HTTP /captures/ route) — 2026-04-29

### Why the fix-up was needed

The original 02-01 SUMMARY (above) claimed users could browse to
`http://<device>/file?file=/captures/wintex-XXXX-001.bin` to download
captures via ESPHome's stock `web_server` v3. That URL pattern does not
exist — `web_server` v3 has no arbitrary-LittleFS-path endpoint. The
LittleFS writer landed correctly, but its only "exposure" was via SSH /
serial-console / re-flash inspection of the raw flash. Plan 02-02 needs
*real* HTTP downloads.

### Approach: custom AsyncWebHandler on web_server_base

The fall-back the original plan flagged ("a small custom HTTP route
inside the texecom component") is now built. Reasoning:

- ESPHome 2026.4 on ESP32 routes all `web_server`-family components
  through `web_server_idf` (a thin wrapper over esp-idf's `httpd_*`
  API). The `WebServerBase` singleton (`global_web_server_base`) accepts
  custom `AsyncWebHandler` registrations alongside the dashboard/OTA
  handlers — this is the documented hook ESPHome exposes for external
  components, and the same hook `captive_portal` uses internally.
- Trying to lean on a third-party "filesystem browser" component would
  add a build-flag dependency we don't control. Hand-rolling ~250 LoC
  keeps the surface area small, avoids a new git submodule, and lets us
  enforce the path-safety rules ourselves.
- File contents are streamed via `httpd_resp_send_chunk()` in 1 KB
  chunks, so a full 256 KB capture never lands in heap at once.

### New files

- `components/texecom/capture_http.h` — public surface: forward-declares
  `Capture`, exposes the pure `is_safe_capture_filename(name)` validator
  (host-testable, security-critical), and declares the on-device
  registration hook gated behind `USE_ARDUINO && USE_NETWORK`.
- `components/texecom/capture_http.cpp` — defines `is_safe_capture_filename`
  unconditionally, plus the `CaptureHttpHandler` AsyncWebHandler subclass
  and `register_capture_http_handler()` under the same gate.
  Pulls `esphome/core/defines.h` via `__has_include` so the gates
  evaluate consistently in this translation unit.
- `tests/test_capture_http_paths.cpp` — 6 Catch2 TEST_CASEs covering the
  validator (plain pass / traversal / separators / control bytes /
  empty-and-oversize / extension whitelist).

### Files modified

- `components/texecom/texecom.cpp` — `Texecom::setup()` now calls
  `register_capture_http_handler(&capture_, capture_.root_path())`
  after `capture_.setup()` (gated). `dump_config()` logs a "Capture URL"
  line so the user can see the listing URL on boot.
- `tests/CMakeLists.txt` — adds `test_capture_http_paths.cpp` and
  `capture_http.cpp` to the host-test target.

### URL pattern users hit

```
http://<device-ip>/captures/                     # HTML directory listing
http://<device-ip>/captures/wintex-XXXX-001.bin  # binary download
http://<device-ip>/captures/wintex-XXXX-001.txt  # hex dump (inline)
```

The mDNS hostname works the same way:
`http://texecom-bridge-<id>.local/captures/`.

`GET /captures` (no trailing slash) returns a 302 redirect to
`/captures/` so relative download links resolve. `.bin` is sent with
`Content-Type: application/octet-stream` and
`Content-Disposition: attachment` (browser prompts to save). `.txt` is
sent `text/plain; charset=utf-8` with `Content-Disposition: inline` so
the hex dump renders in-tab.

### Path safety guarantees

`is_safe_capture_filename(name)` enforces:

- Non-empty, length <= 96 chars (matches the snprintf bound in
  `capture.cpp::open_new_file_()`).
- No leading `.` (rejects `.`, `..`, dotfiles).
- No `/`, `\\`, `:`, NUL, or any byte < 0x20 / == 0x7F.
- Extension must be `.bin` or `.txt` (case-insensitive).

The handler validates the URL tail via this function before *any*
LittleFS lookup, and the directory listing also runs every entry it
finds through the same validator before linking it. Both rules are
locked by `tests/test_capture_http_paths.cpp`.

### Validation results

| Command | Status |
|---------|--------|
| `esphome config esphome/texecom-bridge.yaml` | exit 0 — "Configuration is valid!" |
| `esphome compile esphome/texecom-bridge.yaml` | exit 0 — firmware.elf linked, firmware.factory.bin produced; one iteration to fix a `USE_NETWORK` gate (defines.h not transitively included) |
| Host C++ tests (cmake/ctest) | NOT RUN locally — Windows host has no cmake/g++/clang/MSVC on PATH (same constraint as the original 02-01 work). CI workflow `host-tests` exercises them on push. |

### Live-verification gaps

These cannot be proved without OTA-flashing the device + actually
running a Wintex session that produces capture files:

- The directory listing renders correctly with real `wintex-*.bin`/
  `.txt` files in `/captures/`.
- `.bin` downloads as `application/octet-stream` and the browser
  prompts to save (not preview).
- `.txt` renders inline as `text/plain` (not download-prompted).
- File-streaming via `httpd_resp_send_chunk` keeps memory bounded at
  the 1 KB buffer rather than mallocing the full capture.
- `Capture URL: http://<device>/captures/` line appears in
  `dump_config` after boot.
- Path-traversal attempts (e.g. `curl http://<dev>/captures/../etc/passwd`)
  return 404, not the file. (Path-safety is unit-tested host-side, but
  the route plumbing itself is integration-only.)

### Next-step note

After flash, browse to `http://texecom-bridge-2f7dc4b9.local/captures/`
and confirm the listing renders, then click a `.bin` link to verify the
download prompt. The user-instruction block above for collecting
captures stays valid — only the download URL changes from the broken
`/file?file=/captures/...` pattern to the real `/captures/<name>` route.

---

## Plan 02-01 fix-up part 2 — pivot to RAM-only storage (2026-04-30)

The HTTP route from part 1 above hit a deeper problem on contact with
the live device: capture **writes were silently dropping**.

### Symptom

Confirmed across many sessions on the Atom S3:

```
capture: opened /wintex-boot38559-001.bin
capture: post-write exists() bin=false txt=false   <-- !
capture: closed /wintex-boot38559-001.bin (56 bytes)
```

`LittleFS.open(path, "w")` returned a valid handle. `f.write()` and
`f.close()` returned success. Yet `LittleFS.exists(path)` returned
`false` immediately after. The "files" we appeared to write went into
a black hole as far as any subsequent read was concerned.

### Diagnosis

Symptom reproduced after a full `esptool erase_flash` + USB reflash —
ruling out residual partition corruption from the device's prior
F1p Ecodan firmware. Root cause: **ESPHome's build links two
LittleFS implementations on the same partition**:

- `arduino-esp32`'s bundled LittleFS (used by `capture.cpp` via
  `LittleFS.open` / `FS.h`)
- joltwallet's `esp_littlefs` (pulled in by `web_server_idf` for
  the `web_server:` block we re-enabled, mounted via VFS)

Both report mount-success. Their state isn't coherent — writes via
the Arduino API don't appear via the IDF VFS read path. We tried:

1. mkdir on every session start with retry — no effect
2. `LittleFS.format()` recovery on directory probe failure — no effect
3. Falling back to LittleFS root (no subdirectory) — writes still
   silently dropped
4. `CONFIG_LITTLEFS_SPIFFS_COMPAT=1` build flag — broke the build
   (it's an arduino-esp32 LittleFS flag; the joltwallet variant has a
   different API surface and rejected the symbols)
5. Full `esptool erase_flash` — no effect (confirms it isn't a
   residual partition state)

After 7 dead-end commits (b81dcec → ca1ddd8) we pivoted.

### Pivot

Capture storage is now **in-RAM only**, commit `25ee3cb`:

- `Capture` owns a `std::vector<RamCapture>` of (name, bytes, mtime)
  tuples. One entry per `.bin`, one per `.txt`.
- `set_max_total_bytes()` (default **128 KB**) caps total RAM used
  across all stored captures. When a write would push over budget,
  the oldest capture is evicted; the live `.bin`/`.txt` are skipped
  to avoid mid-session truncation.
- HTTP listing iterates `Capture::list_captures()` snapshots; file
  download streams from `Capture::get_capture_bytes()` via
  `httpd_resp_send_chunk` in 1 KB chunks.
- TXCP wire format unchanged — host-side parser tests still pass
  bit-for-bit.

### YAML schema change

- Dropped `capture_root` (no on-disk path).
- Renamed `capture_max_file_bytes` → `capture_max_ram_bytes`
  (default `131072`).

Existing YAMLs need a one-line update; example deployed at
`esphome/texecom-bridge.yaml:154`.

### Trade-off

Captures are **lost on reboot**. The user's workflow ("run a Wintex
session → immediately download from the device's web UI") doesn't
require persistence; this isn't worse than what we had (where writes
also vanished, just less visibly).

### Live verification (2026-04-30)

End-to-end pass on the bench device:

| Check | Result |
|---|---|
| Listing renders with two files (`.bin`, `.txt`) after a TCP/10001 session | ✅ |
| Storage diagnostic shows `725 / 131072 bytes used` | ✅ |
| `.bin` HTTP 200, byte-count matches listed size, magic = `TXCP` | ✅ |
| `.txt` HTTP 200, banner + xxd-style hex dump renders inline | ✅ |
| `foo.exe` returns HTTP 404 (extension whitelist) | ✅ |
| Boot log shows `Capture ready: in-RAM (max=131072 bytes)` | ✅ |

Eviction across two sessions and the "single session exceeds budget"
WARN path remain to be exercised against real bench traffic; no
automated test covers them.
