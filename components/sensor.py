"""ZW101 Sensor 平台"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    ICON_FINGERPRINT,
    STATE_CLASS_MEASUREMENT,
)

from . import ZW101Component, zw101_ns

DEPENDENCIES = ["zw101"]

CONF_ZW101_ID = "zw101_id"
CONF_MATCH_SCORE = "match_score"
CONF_MATCH_ID = "match_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ZW101_ID): cv.use_id(ZW101Component),
        cv.Optional(CONF_MATCH_SCORE): sensor.sensor_schema(
            icon="mdi:percent",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_MATCH_ID): sensor.sensor_schema(
            icon=ICON_FINGERPRINT,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)


async def to_code(config):
    """生成 sensor 代码"""
    parent = await cg.get_variable(config[CONF_ZW101_ID])

    if CONF_MATCH_SCORE in config:
        sens = await sensor.new_sensor(config[CONF_MATCH_SCORE])
        cg.add(parent.set_match_score_sensor(sens))

    if CONF_MATCH_ID in config:
        sens = await sensor.new_sensor(config[CONF_MATCH_ID])
        cg.add(parent.set_match_id_sensor(sens))
