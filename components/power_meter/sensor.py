import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import mcp3008, sensor
from esphome.const import (
    CONF_APPARENT_POWER,
    CONF_CHANNEL,
    CONF_CURRENT,
    CONF_ID,
    CONF_POWER,
    CONF_POWER_FACTOR,
    DEVICE_CLASS_APPARENT_POWER,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    ICON_CURRENT_AC,
    ICON_FLASH,
    STATE_CLASS_MEASUREMENT,
    UNIT_AMPERE,
    UNIT_EMPTY,
    UNIT_VOLT_AMPS,
    UNIT_WATT,
)

from . import CONF_MCP3008_ID, PowerMeter, power_meter_ns

CONF_POWER_METER_ID = "power_meter_id"
CONF_BURDEN_RESISTOR = "burden_resistor"
CONF_CT_RATIO = "ct_ratio"
CONF_PHASE_CORRECTION_DEG = "phase_correction_deg"

PowerMeterChannel = power_meter_ns.class_("PowerMeterChannel")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PowerMeterChannel),
        cv.GenerateID(CONF_POWER_METER_ID): cv.use_id(PowerMeter),
        cv.Required(CONF_MCP3008_ID): cv.use_id(mcp3008.MCP3008),
        cv.Required(CONF_CHANNEL): cv.int_range(min=0, max=7),
        cv.Required(CONF_BURDEN_RESISTOR): cv.positive_float,
        cv.Required(CONF_CT_RATIO): cv.positive_float,
        cv.Optional(CONF_PHASE_CORRECTION_DEG, default=0.0): cv.float_range(
            min=-10.0, max=10.0
        ),
        cv.Optional(CONF_CURRENT): sensor.sensor_schema(
            unit_of_measurement=UNIT_AMPERE,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_CURRENT,
            state_class=STATE_CLASS_MEASUREMENT,
            icon=ICON_CURRENT_AC,
        ),
        cv.Optional(CONF_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
            icon=ICON_FLASH,
        ),
        cv.Optional(CONF_APPARENT_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT_AMPS,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_APPARENT_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
            icon=ICON_FLASH,
        ),
        cv.Optional(CONF_POWER_FACTOR): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_POWER_FACTOR,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_POWER_METER_ID])
    adc = await cg.get_variable(config[CONF_MCP3008_ID])
    cg.add(var.set_adc(adc))
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_burden_resistor(config[CONF_BURDEN_RESISTOR]))
    cg.add(var.set_ct_ratio(config[CONF_CT_RATIO]))
    cg.add(var.set_phase_correction_deg(config[CONF_PHASE_CORRECTION_DEG]))
    if CONF_CURRENT in config:
        s = await sensor.new_sensor(config[CONF_CURRENT])
        cg.add(var.set_current_sensor(s))
    if CONF_POWER in config:
        s = await sensor.new_sensor(config[CONF_POWER])
        cg.add(var.set_power_sensor(s))
    if CONF_APPARENT_POWER in config:
        s = await sensor.new_sensor(config[CONF_APPARENT_POWER])
        cg.add(var.set_apparent_power_sensor(s))
    if CONF_POWER_FACTOR in config:
        s = await sensor.new_sensor(config[CONF_POWER_FACTOR])
        cg.add(var.set_power_factor_sensor(s))
    cg.add(parent.add_channel(var))
