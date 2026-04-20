"""ESPHome external component: texecom.

Bridges a Texecom Premier 24 alarm panel UART to a TCP listener so Wintex
(or any raw TCP client) can talk to the panel over LAN.

Phase 1 scope: transparent byte pipe on TCP port 10001, single client,
Monitor/Bridge session state machine. Phase 2 wires in decode/encode;
Phase 3 wires in MQTT auto-discovery.

Plan 02-01 adds on-device capture of every byte the bridge moves,
persisted to LittleFS and downloadable via the ESPHome web_server.
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
CONF_CAPTURE_MAX_FILE_BYTES = "capture_max_file_bytes"
CONF_CAPTURE_ROOT = "capture_root"

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
        # connected (Bridge mode), rotate at 256 KB, store under
        # /captures on the device's LittleFS.
        cv.Optional(CONF_RECORD_WHEN, default="bridge"): cv.enum(
            RECORD_MODES, lower=True
        ),
        cv.Optional(CONF_CAPTURE_MAX_FILE_BYTES, default=262144): cv.int_range(
            min=4096, max=4 * 1024 * 1024
        ),
        cv.Optional(CONF_CAPTURE_ROOT, default="/captures"): cv.string_strict,
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
    cg.add(var.set_capture_max_file_bytes(config[CONF_CAPTURE_MAX_FILE_BYTES]))
    cg.add(var.set_capture_root(config[CONF_CAPTURE_ROOT]))

    # TCP listener uses ESPHome's native `socket` component (declared in
    # DEPENDENCIES). No third-party TCP library is required — the
    # previous callback-driven TCP library has been removed.

    # LittleFS access for capture persistence. FS and LittleFS are
    # bundled with the Arduino-ESP32 core but ESPHome's external
    # components don't pick them up automatically; declaring them
    # here puts the framework headers on the include path so
    # capture.cpp's `#include <LittleFS.h>` resolves.
    cg.add_library("FS", None)
    cg.add_library("LittleFS", None)
