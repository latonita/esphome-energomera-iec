import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import (
    CONF_INDEX,
)
from . import (
    EnergomeraIec,
    CONF_ENERGOMERA_IEC_ID,
    energomera_iec_ns,
    validate_request_format,
    CONF_REQUEST,
    DEFAULTS_MAX_SENSOR_INDEX,
)

AUTO_LOAD = ["energomera_iec"]

EnergomeraIecTextSensor = energomera_iec_ns.class_(
    "EnergomeraIecTextSensor", text_sensor.TextSensor
)

CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(
        EnergomeraIecTextSensor,
    ).extend(
        {
            cv.GenerateID(CONF_ENERGOMERA_IEC_ID): cv.use_id(EnergomeraIec),
            cv.Required(CONF_REQUEST): validate_request_format,
            cv.Optional(CONF_INDEX, default=1): cv.int_range(
                min=1, max=DEFAULTS_MAX_SENSOR_INDEX
            ),
        }
    ),
    cv.has_exactly_one_key(CONF_REQUEST),
)


async def to_code(config):
    component = await cg.get_variable(config[CONF_ENERGOMERA_IEC_ID])
    var = await text_sensor.new_text_sensor(config)

    if CONF_REQUEST in config:
        cg.add(var.set_request(config[CONF_REQUEST]))

    cg.add(var.set_index(config[CONF_INDEX]))

    cg.add(component.register_sensor(var))
