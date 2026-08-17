import esphome.codegen as cg
import esphome.config_validation as cv
# Alias sur `sensor` pour eviter le shadowing par ce fichier lorsque
# __init__.py importe esphome.components.sensor (voir memoire ESPHome).
from esphome.components import sensor as sensor_cp
from esphome.const import (
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_EMPTY,
    UNIT_KILOWATT_HOURS,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_WATT,
)

from . import CONF_CUMULUS_ID, CumulusController

CONF_HEATER_LEVEL = "heater_level"
CONF_HEATER_POWER = "heater_power"
CONF_STORED_ENERGY = "stored_energy"
CONF_ENERGY_TODAY = "energy_today"
CONF_PV_BUDGET = "pv_budget"
CONF_PV_SURPLUS_USED = "pv_surplus_used"
CONF_TIME_TO_TARGET = "time_to_target"
CONF_DUTY_CYCLE_1H = "duty_cycle_1h"
CONF_STANDBY_LOSS = "standby_loss_rate"
CONF_DAYS_SINCE_LEGIONELLA = "days_since_legionella"
CONF_OVERHEAT_COUNT = "overheat_count"
CONF_PUMP_DUTY = "pump_duty"
CONF_EFFECTIVE_TARGET = "effective_target"
CONF_DYNAMIC_OFFSET = "dynamic_offset"
CONF_ENERGY_DEFICIT_FORECAST = "energy_deficit_forecast"
CONF_HORIZON_AVAILABLE = "horizon_available"
CONF_PEAK_WINDOW_ACTIVE = "peak_window_active"
CONF_ELEMENT_HEALTH = "element_health"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_CUMULUS_ID): cv.use_id(CumulusController),
        cv.Optional(CONF_HEATER_LEVEL): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:fire",
        ),
        cv.Optional(CONF_HEATER_POWER): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:flash",
        ),
        cv.Optional(CONF_STORED_ENERGY): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=2,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:battery-charging-high",
        ),
        cv.Optional(CONF_ENERGY_TODAY): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            icon="mdi:counter",
        ),
        cv.Optional(CONF_PV_BUDGET): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=3,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:solar-power",
        ),
        cv.Optional(CONF_PV_SURPLUS_USED): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=3,
            device_class=DEVICE_CLASS_ENERGY,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            icon="mdi:sun-thermometer",
        ),
        cv.Optional(CONF_TIME_TO_TARGET): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_MINUTE,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:timer-sand",
        ),
        cv.Optional(CONF_DUTY_CYCLE_1H): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:chart-timeline-variant",
        ),
        cv.Optional(CONF_STANDBY_LOSS): sensor_cp.sensor_schema(
            unit_of_measurement="°C/h",
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:thermometer-minus",
        ),
        cv.Optional(CONF_DAYS_SINCE_LEGIONELLA): sensor_cp.sensor_schema(
            unit_of_measurement="d",
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:calendar-clock",
        ),
        cv.Optional(CONF_OVERHEAT_COUNT): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            icon="mdi:alert-octagon",
        ),
        cv.Optional(CONF_PUMP_DUTY): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:pump",
        ),
        cv.Optional(CONF_EFFECTIVE_TARGET): sensor_cp.sensor_schema(
            unit_of_measurement="°C",
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:thermometer-check",
        ),
        cv.Optional(CONF_DYNAMIC_OFFSET): sensor_cp.sensor_schema(
            unit_of_measurement="°C",
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:thermometer-plus",
        ),
        cv.Optional(CONF_ENERGY_DEFICIT_FORECAST): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:calendar-alert",
        ),
        cv.Optional(CONF_HORIZON_AVAILABLE): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_KILOWATT_HOURS,
            accuracy_decimals=2,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:weather-sunny",
        ),
        cv.Optional(CONF_PEAK_WINDOW_ACTIVE): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:weather-sunset",
        ),
        cv.Optional(CONF_ELEMENT_HEALTH): sensor_cp.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
            icon="mdi:heart-pulse",
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_CUMULUS_ID])
    for key, setter in (
        (CONF_HEATER_LEVEL, parent.set_heater_level_sensor),
        (CONF_HEATER_POWER, parent.set_heater_power_sensor),
        (CONF_STORED_ENERGY, parent.set_stored_energy_sensor),
        (CONF_ENERGY_TODAY, parent.set_energy_today_sensor),
        (CONF_PV_BUDGET, parent.set_pv_budget_sensor),
        (CONF_PV_SURPLUS_USED, parent.set_pv_surplus_used_sensor),
        (CONF_TIME_TO_TARGET, parent.set_time_to_target_sensor),
        (CONF_DUTY_CYCLE_1H, parent.set_duty_cycle_1h_sensor),
        (CONF_STANDBY_LOSS, parent.set_standby_loss_sensor),
        (CONF_DAYS_SINCE_LEGIONELLA, parent.set_days_since_legionella_sensor),
        (CONF_OVERHEAT_COUNT, parent.set_overheat_count_sensor),
        (CONF_PUMP_DUTY, parent.set_pump_duty_sensor),
        (CONF_EFFECTIVE_TARGET, parent.set_effective_target_sensor),
        (CONF_DYNAMIC_OFFSET, parent.set_dynamic_offset_sensor),
        (CONF_ENERGY_DEFICIT_FORECAST, parent.set_energy_deficit_forecast_sensor),
        (CONF_HORIZON_AVAILABLE, parent.set_horizon_available_sensor),
        (CONF_PEAK_WINDOW_ACTIVE, parent.set_peak_window_active_sensor),
        (CONF_ELEMENT_HEALTH, parent.set_element_health_sensor),
    ):
        if cfg := config.get(key):
            s = await sensor_cp.new_sensor(cfg)
            cg.add(setter(s))
