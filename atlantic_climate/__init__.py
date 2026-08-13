# External component pilotant un thermostat/chaudiere Atlantic via bus UART proprietaire.
import esphome.codegen as cg

# Alias pour eviter le shadowing par le sous-module `climate.py` (voir memoire ESPHome).
from esphome.components import climate as climate_cp
from esphome.components import uart

CODEOWNERS = ["@local"]

atlantic_climate_ns = cg.esphome_ns.namespace("atlantic_climate")
AtlanticClimate = atlantic_climate_ns.class_(
    "AtlanticClimate",
    climate_cp.Climate,
    cg.PollingComponent,
    uart.UARTDevice,
)

CONF_ADDRESS = "address"
CONF_DEBUG_FRAMES = "debug_frames"
CONF_REFERENCE_SENSOR = "reference_sensor"
CONF_ROOMS = "rooms"
CONF_ROOM_CURRENT = "current"
CONF_ROOM_TARGET = "target"
CONF_ROOM_STATE = "state"
CONF_DEADBAND = "deadband"
CONF_DELTA_MAX = "delta_max"
CONF_IDLE_OFFSET = "idle_offset"
CONF_MANUAL_HOLD = "manual_hold"
CONF_WATCHDOG_TIMEOUT = "watchdog_timeout"
CONF_SAFE_TARGET = "safe_target"
CONF_ATLANTIC_CLIMATE_ID = "atlantic_climate_id"
