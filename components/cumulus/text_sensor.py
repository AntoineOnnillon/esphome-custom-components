import esphome.codegen as cg
import esphome.config_validation as cv
# Alias sur `text_sensor` par coherence avec les autres plateformes (memoire ESPHome).
from esphome.components import text_sensor as text_sensor_cp

from . import CONF_CUMULUS_ID, CumulusController

CONF_MODE = "mode"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_CUMULUS_ID): cv.use_id(CumulusController),
        cv.Optional(CONF_MODE): text_sensor_cp.text_sensor_schema(
            icon="mdi:state-machine",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_CUMULUS_ID])
    if cfg := config.get(CONF_MODE):
        s = await text_sensor_cp.new_text_sensor(cfg)
        cg.add(parent.set_mode_text_sensor(s))
