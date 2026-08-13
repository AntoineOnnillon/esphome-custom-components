import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate, sensor, text_sensor, uart

from . import (
    AtlanticClimate,
    CONF_ADDRESS,
    CONF_DEADBAND,
    CONF_DEBUG_FRAMES,
    CONF_DELTA_MAX,
    CONF_IDLE_OFFSET,
    CONF_MANUAL_HOLD,
    CONF_REFERENCE_SENSOR,
    CONF_ROOM_CURRENT,
    CONF_ROOM_STATE,
    CONF_ROOM_TARGET,
    CONF_ROOMS,
    CONF_SAFE_TARGET,
    CONF_SNIFF_ALL_FRAMES,
    CONF_WATCHDOG_TIMEOUT,
)

DEPENDENCIES = ["uart"]

ROOM_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ROOM_CURRENT): cv.use_id(sensor.Sensor),
        cv.Required(CONF_ROOM_TARGET): cv.use_id(sensor.Sensor),
        cv.Optional(CONF_ROOM_STATE): cv.use_id(text_sensor.TextSensor),
    }
)

CONFIG_SCHEMA = (
    climate.climate_schema(AtlanticClimate)
    .extend(
        {
            cv.Optional(CONF_ADDRESS, default=7): cv.int_range(min=1, max=126),
            cv.Optional(CONF_DEBUG_FRAMES, default=False): cv.boolean,
            cv.Optional(CONF_SNIFF_ALL_FRAMES, default=False): cv.boolean,
            cv.Optional(CONF_REFERENCE_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_ROOMS): cv.ensure_list(ROOM_SCHEMA),
            cv.Optional(CONF_DEADBAND, default=0.2): cv.float_range(min=0.05, max=5),
            cv.Optional(CONF_DELTA_MAX, default=6.0): cv.float_range(min=0.5, max=15),
            cv.Optional(CONF_IDLE_OFFSET, default=-0.5): cv.float_range(min=-3, max=3),
            cv.Optional(CONF_MANUAL_HOLD, default="15min"): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_WATCHDOG_TIMEOUT, default="10min"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SAFE_TARGET, default=18.0): cv.float_range(min=15, max=28),
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
    cg.add(var.set_debug_frames(config[CONF_DEBUG_FRAMES]))
    cg.add(var.set_sniff_all_frames(config[CONF_SNIFF_ALL_FRAMES]))

    if (ref_id := config.get(CONF_REFERENCE_SENSOR)) is not None:
        ref = await cg.get_variable(ref_id)
        cg.add(var.set_reference_sensor(ref))

    for room in config.get(CONF_ROOMS, []):
        cur = await cg.get_variable(room[CONF_ROOM_CURRENT])
        tgt = await cg.get_variable(room[CONF_ROOM_TARGET])
        state = None
        if (state_id := room.get(CONF_ROOM_STATE)) is not None:
            state = await cg.get_variable(state_id)
        cg.add(var.add_room(cur, tgt, state or cg.nullptr))

    cg.add(var.set_deadband(config[CONF_DEADBAND]))
    cg.add(var.set_delta_max(config[CONF_DELTA_MAX]))
    cg.add(var.set_idle_offset(config[CONF_IDLE_OFFSET]))
    cg.add(var.set_manual_hold_ms(config[CONF_MANUAL_HOLD]))
    cg.add(var.set_watchdog_timeout_ms(config[CONF_WATCHDOG_TIMEOUT]))
    cg.add(var.set_safe_target(config[CONF_SAFE_TARGET]))
