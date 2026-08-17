# External component: controleur de cumulus solaire.
# Pilote une resistance de chauffe-eau via une sortie float (SSR/PWM/DAC),
# controle une pompe de circulation optionnelle, applique une strategie PV
# (budget accumule ou modulation continue) et gere l'anti-legionellose + les
# securites de surchauffe.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import output
from esphome.components import sensor as sensor_cp
from esphome.components import switch as switch_cp
from esphome.const import CONF_ID

CODEOWNERS = ["@local"]

cumulus_ns = cg.esphome_ns.namespace("cumulus")
CumulusController = cumulus_ns.class_("CumulusController", cg.PollingComponent)

# Cles de configuration partagees entre __init__.py et les fichiers de plateforme.
CONF_CUMULUS_ID = "cumulus_id"

CONF_HEATER_OUTPUT = "heater_output"
CONF_PUMP_SWITCH = "pump_switch"

CONF_TOP_TEMPERATURE = "top_temperature"
CONF_ELEMENT_TEMPERATURE = "element_temperature"
CONF_OUTLET_TEMPERATURE = "outlet_temperature"
CONF_SSR_TEMPERATURE = "ssr_temperature"

CONF_PV_SURPLUS = "pv_surplus"
CONF_PV_FORECAST = "pv_forecast"
CONF_PV_FORECAST_D1 = "pv_forecast_d1"
CONF_PV_FORECAST_D2 = "pv_forecast_d2"
CONF_CONSUMPTION_FORECAST = "consumption_forecast"
CONF_PV_PEAK_HOUR = "pv_peak_hour"
CONF_HEATER_POWER_MEASURED = "heater_power_measured"
CONF_GRID_EXPORT_POWER = "grid_export_power"
CONF_GRID_IMPORT_POWER = "grid_import_power"

CONF_TANK_VOLUME_L = "tank_volume_l"
CONF_ELEMENT_POWER_W = "element_power_w"
CONF_TARGET_TEMPERATURE = "target_temperature"
CONF_MIN_TEMPERATURE = "min_temperature"
CONF_MAX_TEMPERATURE = "max_temperature"
CONF_HYSTERESIS = "hysteresis"
CONF_OVERHEAT_TEMPERATURE = "overheat_temperature"
CONF_SSR_OVERHEAT_TEMPERATURE = "ssr_overheat_temperature"
CONF_PUMP_DELTA_ON = "pump_delta_on"
CONF_PUMP_DELTA_OFF = "pump_delta_off"
CONF_PUMP_TARGET_TEMPERATURE = "pump_target_temperature"
CONF_PUMP_MIN_INTERVAL = "pump_min_interval"
CONF_LEGIONELLA_TARGET = "legionella_target"
CONF_LEGIONELLA_INTERVAL_HOURS = "legionella_interval_hours"
CONF_LEGIONELLA_HOLD_MINUTES = "legionella_hold_minutes"
CONF_PV_SURPLUS_THRESHOLD = "pv_surplus_threshold"
CONF_PV_BURST_HYSTERESIS_WH = "pv_burst_hysteresis_wh"
CONF_PV_PREFER_BURST = "pv_prefer_burst"
CONF_GRID_IMPORT_SAFETY_MARGIN = "grid_import_safety_margin"
CONF_WATCHDOG_TIMEOUT = "watchdog_timeout"

# ---- Planificateur multi-jours ----
CONF_PLANNER = "planner"
CONF_PLANNER_ENABLE = "enable"
CONF_WAIT_FOR_PEAK = "wait_for_peak"
CONF_PEAK_WINDOW_START_H = "peak_window_start_hour"
CONF_PEAK_WINDOW_END_H = "peak_window_end_hour"
CONF_PEAK_WINDOW_PAD_BEFORE_H = "peak_window_pad_before_hours"
CONF_PEAK_WINDOW_PAD_AFTER_H = "peak_window_pad_after_hours"
CONF_DAILY_CONSUMPTION_KWH = "daily_consumption_kwh"
CONF_HORIZON_DAYS = "horizon_days"
CONF_BOOST_OFFSET_MAX = "boost_offset_max"
CONF_DEFICIT_OFFSET_MAX = "deficit_offset_max"

PLANNER_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_PLANNER_ENABLE, default=True): cv.boolean,
        cv.Optional(CONF_WAIT_FOR_PEAK, default=True): cv.boolean,
        cv.Optional(CONF_PEAK_WINDOW_START_H, default=10): cv.int_range(min=0, max=23),
        cv.Optional(CONF_PEAK_WINDOW_END_H, default=16): cv.int_range(min=1, max=24),
        cv.Optional(CONF_PEAK_WINDOW_PAD_BEFORE_H, default=1): cv.int_range(min=0, max=6),
        cv.Optional(CONF_PEAK_WINDOW_PAD_AFTER_H, default=2): cv.int_range(min=0, max=6),
        cv.Optional(CONF_DAILY_CONSUMPTION_KWH, default=8.0): cv.float_range(min=0.5, max=50.0),
        cv.Optional(CONF_HORIZON_DAYS, default=2): cv.int_range(min=1, max=3),
        cv.Optional(CONF_BOOST_OFFSET_MAX, default=8.0): cv.float_range(min=0, max=20),
        cv.Optional(CONF_DEFICIT_OFFSET_MAX, default=5.0): cv.float_range(min=0, max=20),
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CumulusController),
            cv.Required(CONF_HEATER_OUTPUT): cv.use_id(output.FloatOutput),
            cv.Optional(CONF_PUMP_SWITCH): cv.use_id(switch_cp.Switch),
            # Sondes NTC (toutes optionnelles).
            cv.Optional(CONF_TOP_TEMPERATURE): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_ELEMENT_TEMPERATURE): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_OUTLET_TEMPERATURE): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_SSR_TEMPERATURE): cv.use_id(sensor_cp.Sensor),
            # Donnees externes (Home Assistant).
            cv.Optional(CONF_PV_SURPLUS): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_PV_FORECAST): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_PV_FORECAST_D1): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_PV_FORECAST_D2): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_CONSUMPTION_FORECAST): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_PV_PEAK_HOUR): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_HEATER_POWER_MEASURED): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_GRID_EXPORT_POWER): cv.use_id(sensor_cp.Sensor),
            cv.Optional(CONF_GRID_IMPORT_POWER): cv.use_id(sensor_cp.Sensor),
            # Configuration ballon.
            cv.Optional(CONF_TANK_VOLUME_L, default=200.0): cv.float_range(min=10, max=1000),
            cv.Optional(CONF_ELEMENT_POWER_W, default=2400.0): cv.float_range(
                min=200, max=10000
            ),
            cv.Optional(CONF_TARGET_TEMPERATURE, default=55.0): cv.float_range(
                min=30, max=75
            ),
            cv.Optional(CONF_MIN_TEMPERATURE, default=40.0): cv.float_range(min=20, max=60),
            cv.Optional(CONF_MAX_TEMPERATURE, default=65.0): cv.float_range(min=40, max=85),
            cv.Optional(CONF_HYSTERESIS, default=4.0): cv.float_range(min=0.5, max=15),
            cv.Optional(CONF_OVERHEAT_TEMPERATURE, default=85.0): cv.float_range(
                min=60, max=95
            ),
            cv.Optional(CONF_SSR_OVERHEAT_TEMPERATURE, default=80.0): cv.float_range(
                min=40, max=110
            ),
            # Pompe.
            cv.Optional(CONF_PUMP_DELTA_ON, default=3.0): cv.float_range(min=0.5, max=15),
            cv.Optional(CONF_PUMP_DELTA_OFF, default=1.0): cv.float_range(min=0.2, max=10),
            cv.Optional(CONF_PUMP_TARGET_TEMPERATURE, default=45.0): cv.float_range(
                min=25, max=65
            ),
            cv.Optional(
                CONF_PUMP_MIN_INTERVAL, default="5min"
            ): cv.positive_time_period_milliseconds,
            # Anti-legionellose.
            cv.Optional(CONF_LEGIONELLA_TARGET, default=65.0): cv.float_range(
                min=55, max=75
            ),
            cv.Optional(CONF_LEGIONELLA_INTERVAL_HOURS, default=168): cv.int_range(
                min=24, max=720
            ),
            cv.Optional(CONF_LEGIONELLA_HOLD_MINUTES, default=30): cv.int_range(
                min=5, max=120
            ),
            # PV.
            cv.Optional(CONF_PV_SURPLUS_THRESHOLD, default=100.0): cv.float_range(
                min=0, max=5000
            ),
            cv.Optional(CONF_PV_BURST_HYSTERESIS_WH, default=100.0): cv.float_range(
                min=0, max=5000
            ),
            cv.Optional(CONF_PV_PREFER_BURST, default=True): cv.boolean,
            cv.Optional(CONF_GRID_IMPORT_SAFETY_MARGIN, default=50.0): cv.float_range(
                min=0, max=1000
            ),
            # Planificateur (bloc optionnel, defauts si absent).
            cv.Optional(CONF_PLANNER, default={}): PLANNER_SCHEMA,
            # Divers.
            cv.Optional(
                CONF_WATCHDOG_TIMEOUT, default="5min"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.polling_component_schema("10s"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    heater = await cg.get_variable(config[CONF_HEATER_OUTPUT])
    cg.add(var.set_heater_output(heater))

    if pump_id := config.get(CONF_PUMP_SWITCH):
        pump = await cg.get_variable(pump_id)
        cg.add(var.set_pump_switch(pump))

    for key, setter in (
        (CONF_TOP_TEMPERATURE, var.set_top_temp_sensor),
        (CONF_ELEMENT_TEMPERATURE, var.set_element_temp_sensor),
        (CONF_OUTLET_TEMPERATURE, var.set_outlet_temp_sensor),
        (CONF_SSR_TEMPERATURE, var.set_ssr_temp_sensor),
        (CONF_PV_SURPLUS, var.set_pv_surplus_sensor),
        (CONF_PV_FORECAST, var.set_pv_forecast_sensor),
        (CONF_PV_FORECAST_D1, var.set_pv_forecast_d1_sensor),
        (CONF_PV_FORECAST_D2, var.set_pv_forecast_d2_sensor),
        (CONF_CONSUMPTION_FORECAST, var.set_consumption_forecast_sensor),
        (CONF_PV_PEAK_HOUR, var.set_peak_hour_sensor),
        (CONF_HEATER_POWER_MEASURED, var.set_heater_power_measured_sensor),
        (CONF_GRID_EXPORT_POWER, var.set_grid_export_sensor),
        (CONF_GRID_IMPORT_POWER, var.set_grid_import_sensor),
    ):
        if sid := config.get(key):
            s = await cg.get_variable(sid)
            cg.add(setter(s))

    cg.add(var.set_tank_volume_l(config[CONF_TANK_VOLUME_L]))
    cg.add(var.set_element_power_w(config[CONF_ELEMENT_POWER_W]))
    cg.add(var.set_default_target_c(config[CONF_TARGET_TEMPERATURE]))
    cg.add(var.set_min_temperature_c(config[CONF_MIN_TEMPERATURE]))
    cg.add(var.set_max_temperature_c(config[CONF_MAX_TEMPERATURE]))
    cg.add(var.set_hysteresis_c(config[CONF_HYSTERESIS]))
    cg.add(var.set_overheat_temperature_c(config[CONF_OVERHEAT_TEMPERATURE]))
    cg.add(var.set_ssr_overheat_c(config[CONF_SSR_OVERHEAT_TEMPERATURE]))
    cg.add(var.set_pump_delta_on_c(config[CONF_PUMP_DELTA_ON]))
    cg.add(var.set_pump_delta_off_c(config[CONF_PUMP_DELTA_OFF]))
    cg.add(var.set_pump_target_c(config[CONF_PUMP_TARGET_TEMPERATURE]))
    cg.add(var.set_pump_min_interval_ms(config[CONF_PUMP_MIN_INTERVAL]))
    cg.add(var.set_legionella_target_c(config[CONF_LEGIONELLA_TARGET]))
    cg.add(var.set_legionella_interval_h(config[CONF_LEGIONELLA_INTERVAL_HOURS]))
    cg.add(var.set_legionella_hold_min(config[CONF_LEGIONELLA_HOLD_MINUTES]))
    cg.add(var.set_pv_surplus_threshold_w(config[CONF_PV_SURPLUS_THRESHOLD]))
    cg.add(var.set_pv_burst_hysteresis_wh(config[CONF_PV_BURST_HYSTERESIS_WH]))
    cg.add(var.set_pv_prefer_burst(config[CONF_PV_PREFER_BURST]))
    cg.add(var.set_grid_import_safety_margin_w(config[CONF_GRID_IMPORT_SAFETY_MARGIN]))
    cg.add(var.set_watchdog_timeout_ms(config[CONF_WATCHDOG_TIMEOUT]))

    planner = config[CONF_PLANNER]
    cg.add(var.set_planner_enabled(planner[CONF_PLANNER_ENABLE]))
    cg.add(var.set_wait_for_peak(planner[CONF_WAIT_FOR_PEAK]))
    cg.add(var.set_peak_window_start_h(planner[CONF_PEAK_WINDOW_START_H]))
    cg.add(var.set_peak_window_end_h(planner[CONF_PEAK_WINDOW_END_H]))
    cg.add(var.set_peak_window_pad_before_h(planner[CONF_PEAK_WINDOW_PAD_BEFORE_H]))
    cg.add(var.set_peak_window_pad_after_h(planner[CONF_PEAK_WINDOW_PAD_AFTER_H]))
    cg.add(var.set_daily_consumption_kwh(planner[CONF_DAILY_CONSUMPTION_KWH]))
    cg.add(var.set_horizon_days(planner[CONF_HORIZON_DAYS]))
    cg.add(var.set_boost_offset_max_c(planner[CONF_BOOST_OFFSET_MAX]))
    cg.add(var.set_deficit_offset_max_c(planner[CONF_DEFICIT_OFFSET_MAX]))
