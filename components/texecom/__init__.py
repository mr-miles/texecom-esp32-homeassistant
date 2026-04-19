"""ESPHome external component: texecom.

Bridges a Texecom Premier 24 alarm panel UART to a TCP listener so Wintex
(or any raw TCP client) can talk to the panel over LAN.

Phase 1 scope: transparent byte pipe on TCP port 10001, single client,
Monitor/Bridge session state machine. Phase 2 wires in decode/encode;
Phase 3 wires in MQTT auto-discovery.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@mr_miles"]
DEPENDENCIES = ["uart", "network"]
AUTO_LOAD = []

CONF_UART_ID = "uart_id"
CONF_TCP_PORT = "tcp_port"

texecom_ns = cg.esphome_ns.namespace("texecom")
Texecom = texecom_ns.class_("Texecom", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Texecom),
        cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Optional(CONF_TCP_PORT, default=10001): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_UART_ID])
    cg.add(var.set_uart_parent(parent))
    cg.add(var.set_tcp_port(config[CONF_TCP_PORT]))

    # AsyncTCP is fetched via esphome.libraries in the YAML.
    cg.add_library("AsyncTCP", None)
