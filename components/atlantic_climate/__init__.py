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
CONF_ATLANTIC_CLIMATE_ID = "atlantic_climate_id"
