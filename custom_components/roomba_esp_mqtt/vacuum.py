from homeassistant.components.vacuum import StateVacuumEntity, VacuumEntityFeature
from homeassistant.const import STATE_IDLE

from .const import DOMAIN, DEFAULT_NAME

async def async_setup_platform(hass, config, async_add_entities, discovery_info=None):
    async_add_entities([RoombaEspMqttVacuum()], True)

class RoombaEspMqttVacuum(StateVacuumEntity):
    _attr_name = DEFAULT_NAME
    _attr_unique_id = "roomba_esp_mqtt_local"
    _attr_supported_features = (
        VacuumEntityFeature.START
        | VacuumEntityFeature.PAUSE
        | VacuumEntityFeature.STOP
        | VacuumEntityFeature.RETURN_HOME
        | VacuumEntityFeature.LOCATE
    )

    def __init__(self):
        self._attr_state = STATE_IDLE
        self._attr_available = True
        self._battery_level = None

    @property
    def battery_level(self):
        return self._battery_level

    async def async_start(self):
        self._attr_state = "cleaning"
        self.async_write_ha_state()

    async def async_pause(self):
        self._attr_state = "paused"
        self.async_write_ha_state()

    async def async_stop(self, **kwargs):
        self._attr_state = "idle"
        self.async_write_ha_state()

    async def async_return_to_base(self, **kwargs):
        self._attr_state = "returning"
        self.async_write_ha_state()

    async def async_locate(self, **kwargs):
        self.async_write_ha_state()