"""ZW101 Switch 平台"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID

from . import ZW101Component, zw101_ns

DEPENDENCIES = ["zw101"]

CONF_ZW101_ID = "zw101_id"
CONF_ENROLL = "enroll"
CONF_CLEAR = "clear"

EnrollSwitch = zw101_ns.class_("EnrollSwitch", switch.Switch, cg.Component)
ClearSwitch = zw101_ns.class_("ClearSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ZW101_ID): cv.use_id(ZW101Component),
        cv.Optional(CONF_ENROLL): switch.switch_schema(
            EnrollSwitch,
            icon="mdi:fingerprint-add",
        ),
        cv.Optional(CONF_CLEAR): switch.switch_schema(
            ClearSwitch,
            icon="mdi:delete-forever",
        ),
    }
)


async def to_code(config):
    """生成 switch 代码"""
    parent = await cg.get_variable(config[CONF_ZW101_ID])

    if CONF_ENROLL in config:
        sw = await switch.new_switch(config[CONF_ENROLL])
        cg.add(sw.set_parent(parent))
        cg.add(parent.set_enroll_switch(sw))

    if CONF_CLEAR in config:
        sw = await switch.new_switch(config[CONF_CLEAR])
        cg.add(sw.set_parent(parent))
        cg.add(parent.set_clear_switch(sw))
