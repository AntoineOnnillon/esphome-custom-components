import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from . import CONF_CUMULUS_ID, CumulusController, cumulus_ns

CONF_ENABLE = "enable"
CONF_BOOST = "boost"
CONF_PV_PRIORITY = "pv_priority"
CONF_LEGIONELLA_NOW = "legionella_now"

CumulusEnableSwitch = cumulus_ns.class_(
    "CumulusEnableSwitch", switch.Switch, cg.Component
)
CumulusBoostSwitch = cumulus_ns.class_(
    "CumulusBoostSwitch", switch.Switch, cg.Component
)
CumulusPvPrioritySwitch = cumulus_ns.class_(
    "CumulusPvPrioritySwitch", switch.Switch, cg.Component
)
CumulusLegionellaNowSwitch = cumulus_ns.class_(
    "CumulusLegionellaNowSwitch", switch.Switch, cg.Component
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_CUMULUS_ID): cv.use_id(CumulusController),
        cv.Optional(CONF_ENABLE): switch.switch_schema(CumulusEnableSwitch),
        cv.Optional(CONF_BOOST): switch.switch_schema(CumulusBoostSwitch),
        cv.Optional(CONF_PV_PRIORITY): switch.switch_schema(CumulusPvPrioritySwitch),
        cv.Optional(CONF_LEGIONELLA_NOW): switch.switch_schema(
            CumulusLegionellaNowSwitch
        ),
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_CUMULUS_ID])
    for key in (CONF_ENABLE, CONF_BOOST, CONF_PV_PRIORITY, CONF_LEGIONELLA_NOW):
        if cfg := config.get(key):
            # new_switch() enregistre deja le composant (cg.register_component en interne).
            var = await switch.new_switch(cfg)
            cg.add(var.set_parent(parent))
