import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

from . import CONF_ATLANTIC_CLIMATE_ID, AtlanticClimate

CONF_AMBIENT_TEMPERATURE = "ambient_temperature"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_CLIMATE_ID): cv.use_id(AtlanticClimate),
        cv.Optional(CONF_AMBIENT_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=2,
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_ATLANTIC_CLIMATE_ID])
    if amb := config.get(CONF_AMBIENT_TEMPERATURE):
        s = await sensor.new_sensor(amb)
        cg.add(parent.set_ambient_temperature_sensor(s))
