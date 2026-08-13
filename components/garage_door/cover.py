import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import cover, i2c

from . import GarageDoorCover

DEPENDENCIES = ["i2c"]

CONF_SCL_MONITOR_PIN = "scl_monitor_pin"
CONF_SDA_MONITOR_PIN = "sda_monitor_pin"

CONFIG_SCHEMA = (
    cover.cover_schema(GarageDoorCover)
    .extend(
        {
            cv.Required(CONF_SCL_MONITOR_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_SDA_MONITOR_PIN): pins.internal_gpio_input_pin_schema,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x00))
)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    scl_pin = await cg.gpio_pin_expression(config[CONF_SCL_MONITOR_PIN])
    sda_pin = await cg.gpio_pin_expression(config[CONF_SDA_MONITOR_PIN])
    cg.add(var.set_scl_monitor_pin(scl_pin))
    cg.add(var.set_sda_monitor_pin(sda_pin))
