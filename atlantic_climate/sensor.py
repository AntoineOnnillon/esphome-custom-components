import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_EMPTY,
)

from . import CONF_ATLANTIC_CLIMATE_ID, AtlanticClimate

CONF_AMBIENT_TEMPERATURE = "ambient_temperature"
CONF_MAX_DEFICIT = "max_deficit"
CONF_COMPUTED_DELTA = "computed_delta"
CONF_ACTIVE_ROOMS = "active_rooms"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_CLIMATE_ID): cv.use_id(AtlanticClimate),
        cv.Optional(CONF_AMBIENT_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=2,
        ),
        cv.Optional(CONF_MAX_DEFICIT): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=2,
            icon="mdi:thermometer-chevron-up",
        ),
        cv.Optional(CONF_COMPUTED_DELTA): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=2,
            icon="mdi:delta",
        ),
        cv.Optional(CONF_ACTIVE_ROOMS): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
            icon="mdi:home-thermometer",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_ATLANTIC_CLIMATE_ID])
    if amb := config.get(CONF_AMBIENT_TEMPERATURE):
        s = await sensor.new_sensor(amb)
        cg.add(parent.set_ambient_temperature_sensor(s))
    if defc := config.get(CONF_MAX_DEFICIT):
        s = await sensor.new_sensor(defc)
        cg.add(parent.set_deficit_sensor(s))
    if delta := config.get(CONF_COMPUTED_DELTA):
        s = await sensor.new_sensor(delta)
        cg.add(parent.set_delta_sensor(s))
    if rooms := config.get(CONF_ACTIVE_ROOMS):
        s = await sensor.new_sensor(rooms)
        cg.add(parent.set_active_rooms_sensor(s))
