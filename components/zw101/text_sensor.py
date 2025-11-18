"""ZW101 Text Sensor 平台"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID

from . import ZW101Component, zw101_ns

DEPENDENCIES = ["zw101"]

CONF_ZW101_ID = "zw101_id"
CONF_STATUS = "status"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_ZW101_ID): cv.use_id(ZW101Component),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:information-outline"
        ),
    }
)


async def to_code(config):
    """生成 text sensor 代码"""
    parent = await cg.get_variable(config[CONF_ZW101_ID])

    if CONF_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(parent.set_status_sensor(sens))
