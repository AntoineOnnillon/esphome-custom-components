import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, uart

from . import AtlanticClimate, CONF_ADDRESS

DEPENDENCIES = ["uart"]

CONFIG_SCHEMA = (
    climate.climate_schema(AtlanticClimate)
    .extend(
        {
            cv.Optional(CONF_ADDRESS, default=7): cv.int_range(min=1, max=126),
        }
    )
    .extend(cv.polling_component_schema("5min"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_address(config[CONF_ADDRESS]))
