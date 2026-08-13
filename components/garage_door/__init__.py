# External component local pour la commande + sniffing I2C du portail.
import esphome.codegen as cg
from esphome.components import cover, i2c

DEPENDENCIES = ["i2c"]

garage_door_ns = cg.esphome_ns.namespace("garage_door")
GarageDoorCover = garage_door_ns.class_(
    "GarageDoorCover", cover.Cover, cg.Component, i2c.I2CDevice
)

CONF_GARAGE_DOOR_ID = "garage_door_id"
