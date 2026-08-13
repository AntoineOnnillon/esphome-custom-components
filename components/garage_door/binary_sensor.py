import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import CONF_GARAGE_DOOR_ID, GarageDoorCover

CONF_OBSTACLE = "obstacle"
CONF_ONLINE = "online"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GARAGE_DOOR_ID): cv.use_id(GarageDoorCover),
        cv.Optional(CONF_OBSTACLE): binary_sensor.binary_sensor_schema(
            device_class="problem",
            icon="mdi:alert-circle",
        ),
        cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
            icon="mdi:lan-connect",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_GARAGE_DOOR_ID])
    if CONF_OBSTACLE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_OBSTACLE])
        cg.add(parent.set_obstacle_binary_sensor(bs))
    if CONF_ONLINE in config:
        bs = await binary_sensor.new_binary_sensor(config[CONF_ONLINE])
        cg.add(parent.set_online_binary_sensor(bs))
