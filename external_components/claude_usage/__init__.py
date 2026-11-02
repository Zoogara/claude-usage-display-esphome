import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import http_request, sensor, text_sensor
from esphome.const import CONF_ID, STATE_CLASS_MEASUREMENT, UNIT_PERCENT

DEPENDENCIES = ["http_request", "wifi"]
AUTO_LOAD = ["sensor", "text_sensor", "json"]

claude_usage_ns = cg.esphome_ns.namespace("claude_usage")
ClaudeUsage = claude_usage_ns.class_("ClaudeUsage", cg.PollingComponent)

CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_SCOPED_MODEL = "scoped_model"

PERCENT_SENSORS = {
    "session_percent": "set_session_percent",
    "weekly_percent": "set_weekly_percent",
    "scoped_percent": "set_scoped_percent",
}
TEXT_SENSORS = {
    "session_reset": "set_session_reset",
    "weekly_reset": "set_weekly_reset",
    "scoped_reset": "set_scoped_reset",
    "scoped_name": "set_scoped_name",
    "plan": "set_plan",
    "status": "set_status",
    "auth_url": "set_auth_url",
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ClaudeUsage),
            cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(
                http_request.HttpRequestComponent
            ),
            # Substring of the weekly per-model bucket to show, e.g. "Fable".
            # Empty = first weekly_scoped bucket the API returns.
            cv.Optional(CONF_SCOPED_MODEL, default=""): cv.string,
            **{
                cv.Optional(k): sensor.sensor_schema(
                    unit_of_measurement=UNIT_PERCENT,
                    accuracy_decimals=0,
                    state_class=STATE_CLASS_MEASUREMENT,
                )
                for k in PERCENT_SENSORS
            },
            **{cv.Optional(k): text_sensor.text_sensor_schema() for k in TEXT_SENSORS},
        }
    )
    .extend(cv.polling_component_schema("5min"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_http(await cg.get_variable(config[CONF_HTTP_REQUEST_ID])))
    cg.add(var.set_scoped_model(config[CONF_SCOPED_MODEL]))
    for key, setter in PERCENT_SENSORS.items():
        if key in config:
            cg.add(getattr(var, setter)(await sensor.new_sensor(config[key])))
    for key, setter in TEXT_SENSORS.items():
        if key in config:
            cg.add(getattr(var, setter)(await text_sensor.new_text_sensor(config[key])))
