"""ZW101 Binary Sensor 平台"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, DEVICE_CLASS_LOCK
from . import ZW101Component, zw101_ns

DEPENDENCIES = ["zw101"]

CONF_ZW101_ID = "zw101_id"

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_LOCK
).extend(
    {
        cv.GenerateID(CONF_ZW101_ID): cv.use_id(ZW101Component),
    }
)


async def to_code(config):
    """生成 binary sensor 代码"""
    parent = await cg.get_variable(config[CONF_ZW101_ID])
    var = await binary_sensor.new_binary_sensor(config)
    cg.add(parent.set_fingerprint_sensor(var))
