# Power meter external component: samples MCP3008 channels in burst to
# compute real / apparent power on multiple CT clamps, sharing one optional
# voltage channel (either on the same or a different MCP3008 chip).
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import mcp3008
from esphome.components import sensor as sensor_cp
from esphome.const import (
    CONF_CHANNEL,
    CONF_ID,
    CONF_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

DEPENDENCIES = ["mcp3008"]
AUTO_LOAD = ["sensor"]
MULTI_CONF = True

CONF_MCP3008_ID = "mcp3008_id"
CONF_SAMPLES = "samples"
CONF_ADC_REFERENCE_VOLTAGE = "adc_reference_voltage"
CONF_VOLTAGE_FACTOR = "voltage_factor"
CONF_NOMINAL_VOLTAGE = "nominal_voltage"
CONF_REFERENCE_SENSOR = "reference_sensor"
CONF_OVERLOAD_SENSOR = "overload_sensor"
CONF_PHASE_CORRECTION_DEG = "phase_correction_deg"
CONF_NOISE_GATE_PCT = "noise_gate_pct"
CONF_SEQUENTIAL_PER_MCP = "sequential_per_mcp"

power_meter_ns = cg.esphome_ns.namespace("power_meter")
PowerMeter = power_meter_ns.class_("PowerMeter", cg.PollingComponent)

VOLTAGE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_MCP3008_ID): cv.use_id(mcp3008.MCP3008),
        cv.Required(CONF_CHANNEL): cv.int_range(min=0, max=7),
        cv.Required(CONF_VOLTAGE_FACTOR): cv.positive_float,
        cv.Optional(CONF_REFERENCE_SENSOR): cv.use_id(sensor_cp.Sensor),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PowerMeter),
        cv.Optional(CONF_SAMPLES, default=200): cv.int_range(min=20, max=10000),
        cv.Optional(CONF_ADC_REFERENCE_VOLTAGE, default=5.0): cv.positive_float,
        cv.Optional(CONF_NOMINAL_VOLTAGE, default=230.0): cv.templatable(
            cv.positive_float
        ),
        cv.Optional(CONF_PHASE_CORRECTION_DEG, default=0.0): cv.float_range(
            min=-10.0, max=10.0
        ),
        cv.Optional(CONF_NOISE_GATE_PCT, default=0.0): cv.float_range(
            min=0.0, max=20.0
        ),
        cv.Optional(CONF_SEQUENTIAL_PER_MCP, default=False): cv.boolean,
        cv.Optional(CONF_VOLTAGE): VOLTAGE_SCHEMA,
        cv.Optional(CONF_OVERLOAD_SENSOR): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:alert-outline",
        ),
    }
).extend(cv.polling_component_schema("10s"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_samples(config[CONF_SAMPLES]))
    cg.add(var.set_adc_reference_voltage(config[CONF_ADC_REFERENCE_VOLTAGE]))
    cg.add(var.set_default_phase_correction_deg(config[CONF_PHASE_CORRECTION_DEG]))
    cg.add(var.set_noise_gate_pct(config[CONF_NOISE_GATE_PCT]))
    cg.add(var.set_sequential_per_mcp(config[CONF_SEQUENTIAL_PER_MCP]))
    tmpl = await cg.templatable(config[CONF_NOMINAL_VOLTAGE], [], float)
    cg.add(var.set_nominal_voltage(tmpl))
    if CONF_VOLTAGE in config:
        vcfg = config[CONF_VOLTAGE]
        adc = await cg.get_variable(vcfg[CONF_MCP3008_ID])
        cg.add(
            var.set_voltage_channel(
                adc, vcfg[CONF_CHANNEL], vcfg[CONF_VOLTAGE_FACTOR]
            )
        )
        if CONF_REFERENCE_SENSOR in vcfg:
            ref = await cg.get_variable(vcfg[CONF_REFERENCE_SENSOR])
            cg.add(var.set_reference_voltage_sensor(ref))
    if CONF_OVERLOAD_SENSOR in config:
        s = await sensor_cp.new_sensor(config[CONF_OVERLOAD_SENSOR])
        cg.add(var.set_overload_sensor(s))
