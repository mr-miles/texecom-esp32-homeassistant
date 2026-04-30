"""ESPHome external component: texecom.

Bridges a Texecom Premier 24 alarm panel UART to a TCP listener so Wintex
(or any raw TCP client) can talk to the panel over LAN.

Phase 1 scope: transparent byte pipe on TCP port 10001, single client,
Monitor/Bridge session state machine. Phase 2 wires in decode/encode;
Phase 3 wires in MQTT auto-discovery.

Plan 02-01 adds on-device capture of every byte the bridge moves. Captures
are stored IN RAM (no LittleFS) and downloadable via the ESPHome
web_server. Cross-reboot persistence is intentionally not provided — the
workflow is "run a Wintex session -> immediately download from /captures/".
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@mr_miles"]
# `socket` pulls in ESPHome's native TCP API (esphome/components/socket/).
# The Plan 01-02 scaffold used a callback-based TCP library here; it was
# swapped out because the upstream forks kept breaking across
# Arduino-ESP32 revisions.
DEPENDENCIES = ["uart", "network", "socket"]
AUTO_LOAD = []

CONF_UART_ID = "uart_id"
CONF_TCP_PORT = "tcp_port"

# Plan 02-01: capture configuration.
CONF_RECORD_WHEN = "record_when"
CONF_CAPTURE_MAX_RAM_BYTES = "capture_max_ram_bytes"

texecom_ns = cg.esphome_ns.namespace("texecom")
Texecom = texecom_ns.class_("Texecom", cg.Component)
CaptureMode = texecom_ns.namespace("Capture").enum("Mode", is_class=True)

# Map YAML strings to the C++ enum values defined in capture.h.
RECORD_MODES = {
    "none": CaptureMode.None_,
    "bridge": CaptureMode.BridgeOnly,
    "monitor": CaptureMode.MonitorOnly,
    "both": CaptureMode.Both,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Texecom),
        cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Optional(CONF_TCP_PORT, default=10001): cv.port,
        # Capture options. Defaults: record only while a Wintex client is
        # connected (Bridge mode), keep up to 128 KB of captures in RAM.
        cv.Optional(CONF_RECORD_WHEN, default="bridge"): cv.enum(
            RECORD_MODES, lower=True
        ),
        cv.Optional(CONF_CAPTURE_MAX_RAM_BYTES, default=131072): cv.int_range(
            min=4096, max=1024 * 1024
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_UART_ID])
    cg.add(var.set_uart_parent(parent))
    cg.add(var.set_tcp_port(config[CONF_TCP_PORT]))

    # Capture wiring (Plan 02-01).
    cg.add(var.set_capture_mode(config[CONF_RECORD_WHEN]))
    cg.add(var.set_capture_max_ram_bytes(config[CONF_CAPTURE_MAX_RAM_BYTES]))

    # TCP listener uses ESPHome's native `socket` component (declared in
    # DEPENDENCIES). No third-party TCP library is required — the
    # previous callback-driven TCP library has been removed.

    # No filesystem dependencies — captures live entirely in heap RAM.
    # The previous FS / LittleFS library declarations were dropped along
    # with the on-disk writer; web_server_idf still pulls in its own
    # esp_littlefs through ESPHome core if the build needs it.
