import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_SECOND,
)

from . import CONF_ATLANTIC_CLIMATE_ID, AtlanticClimate

AUTO_LOAD = ["text_sensor"]

CONF_AMBIENT_TEMPERATURE = "ambient_temperature"
CONF_DEBUG = "debug"

DEBUG_SENSOR_KEYS = (
    ("rx_bytes", "set_rx_bytes_sensor"),
    ("frames_received", "set_frames_received_sensor"),
    ("rejected_frames", "set_rejected_frames_sensor"),
    ("crc_errors", "set_crc_errors_sensor"),
    ("unknown_frames", "set_unknown_frames_sensor"),
    ("ambient_requests", "set_ambient_requests_sensor"),
    ("ambient_responses", "set_ambient_responses_sensor"),
    ("ambient_response_age", "set_ambient_response_age_sensor"),
    ("request_timeouts", "set_request_timeouts_sensor"),
)


def diagnostic_counter_schema():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    )


DEBUG_SCHEMA = cv.Schema(
    {
        cv.Optional(key): (
            sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_MEASUREMENT,
            )
            if key == "ambient_response_age"
            else diagnostic_counter_schema()
        )
        for key, _ in DEBUG_SENSOR_KEYS
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ATLANTIC_CLIMATE_ID): cv.use_id(AtlanticClimate),
        cv.Optional(CONF_AMBIENT_TEMPERATURE): sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=2,
        ),
        cv.Optional(CONF_DEBUG): DEBUG_SCHEMA.extend(
            {
                cv.Optional("last_unknown_payload"): text_sensor.text_sensor_schema(
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                ),
            }
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_ATLANTIC_CLIMATE_ID])
    if amb := config.get(CONF_AMBIENT_TEMPERATURE):
        s = await sensor.new_sensor(amb)
        cg.add(parent.set_ambient_temperature_sensor(s))

    debug = config.get(CONF_DEBUG, {})
    for key, setter_name in DEBUG_SENSOR_KEYS:
        if sensor_config := debug.get(key):
            debug_sensor = await sensor.new_sensor(sensor_config)
            cg.add(getattr(parent, setter_name)(debug_sensor))

    if payload_config := debug.get("last_unknown_payload"):
        payload_sensor = await text_sensor.new_text_sensor(payload_config)
        cg.add(parent.set_last_unknown_payload_sensor(payload_sensor))
