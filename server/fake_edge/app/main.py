from __future__ import annotations

import json
import logging
import os
import time
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

import paho.mqtt.client as mqtt


logging.basicConfig(
    level=os.getenv("LOG_LEVEL", "INFO"),
    format="%(asctime)s %(levelname)s %(name)s %(message)s",
)
LOGGER = logging.getLogger("solar.fake_edge")


@dataclass(slots=True)
class Settings:
    mqtt_host: str = os.getenv("MQTT_HOST", "mosquitto")
    mqtt_port: int = int(os.getenv("MQTT_PORT", "1883"))
    mqtt_topic: str = os.getenv("MQTT_TOPIC", "sensor/solar/voltage")
    mqtt_username: str = os.getenv("MQTT_USERNAME", "")
    mqtt_password: str = os.getenv("MQTT_PASSWORD", "")
    mqtt_client_id: str = os.getenv("FAKE_MQTT_CLIENT_ID", "solar-fake-edge")
    sensor_id: str = os.getenv("FAKE_SENSOR_ID", "pringles_1")
    fake_voltage: float = float(os.getenv("FAKE_VOLTAGE", "0.476"))
    publish_interval_seconds: float = float(os.getenv("FAKE_PUBLISH_INTERVAL_SECONDS", "1.0"))
    sample_count: int = int(os.getenv("FAKE_SAMPLE_COUNT", "5"))


class FakeEdgePublisher:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.started_monotonic = time.monotonic()
        self.connected = False
        self.client = self._create_mqtt_client()

    def _create_mqtt_client(self) -> mqtt.Client:
        callback_api_version = getattr(mqtt, "CallbackAPIVersion", None)
        if callback_api_version is not None:
            client = mqtt.Client(
                callback_api_version.VERSION2,
                client_id=self.settings.mqtt_client_id,
                clean_session=True,
            )
        else:
            client = mqtt.Client(client_id=self.settings.mqtt_client_id, clean_session=True)

        if self.settings.mqtt_username:
            client.username_pw_set(
                username=self.settings.mqtt_username,
                password=self.settings.mqtt_password or None,
            )

        client.enable_logger(LOGGER)
        client.reconnect_delay_set(min_delay=1, max_delay=30)
        client.on_connect = self.on_connect
        client.on_disconnect = self.on_disconnect
        return client

    def on_connect(
        self,
        _client: mqtt.Client,
        _userdata: Any,
        _flags: Any,
        reason_code: Any,
        _properties: Any = None,
    ) -> None:
        if getattr(reason_code, "is_failure", False):
            LOGGER.error("MQTT connection failed with reason code %s", reason_code)
            self.connected = False
            return

        self.connected = True
        LOGGER.info(
            "Fake edge connected to MQTT broker at %s:%s and publishing sensor_id=%s voltage=%.3f",
            self.settings.mqtt_host,
            self.settings.mqtt_port,
            self.settings.sensor_id,
            self.settings.fake_voltage,
        )

    def on_disconnect(
        self,
        _client: mqtt.Client,
        _userdata: Any,
        reason_code: Any,
        _properties: Any = None,
    ) -> None:
        self.connected = False
        LOGGER.warning("Fake edge disconnected from MQTT broker with reason code %s", reason_code)

    def build_payload(self) -> dict[str, Any]:
        value = round(self.settings.fake_voltage, 3)
        return {
            "sensor_id": self.settings.sensor_id,
            "timestamp": datetime.now(UTC).isoformat().replace("+00:00", "Z"),
            "raw_voltage": value,
            "smoothed_voltage": value,
            "min_v": value,
            "max_v": value,
            "mean_v": value,
            "sample_count": self.settings.sample_count,
            "uptime_seconds": int(time.monotonic() - self.started_monotonic),
        }

    def run(self) -> None:
        LOGGER.info("Connecting fake edge to MQTT broker at %s:%s", self.settings.mqtt_host, self.settings.mqtt_port)
        self.client.connect(self.settings.mqtt_host, self.settings.mqtt_port, keepalive=60)
        self.client.loop_start()

        try:
            while True:
                if not self.connected:
                    time.sleep(1.0)
                    continue

                payload = self.build_payload()
                result = self.client.publish(
                    self.settings.mqtt_topic,
                    json.dumps(payload, separators=(",", ":")),
                    qos=1,
                )
                result.wait_for_publish(timeout=5.0)
                if result.rc != mqtt.MQTT_ERR_SUCCESS:
                    LOGGER.warning("Fake edge publish failed with rc=%s", result.rc)
                else:
                    LOGGER.info("Published fake telemetry raw=%.3f timestamp=%s", payload["raw_voltage"], payload["timestamp"])
                time.sleep(self.settings.publish_interval_seconds)
        finally:
            self.client.loop_stop()
            self.client.disconnect()


def main() -> None:
    settings = Settings()
    while True:
        publisher = FakeEdgePublisher(settings)
        try:
            publisher.run()
        except KeyboardInterrupt:
            LOGGER.info("Stopping fake edge publisher")
            return
        except Exception as exc:
            LOGGER.exception("Fake edge crashed and will restart: %s", exc)
            time.sleep(5)


if __name__ == "__main__":
    main()
