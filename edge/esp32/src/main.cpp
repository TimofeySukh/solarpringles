#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>

#include "secrets.h"

namespace {

constexpr uint8_t DISPLAY_SDA = 18;
constexpr uint8_t DISPLAY_SCL = 19;
constexpr uint8_t SCREEN_WIDTH = 128;
constexpr uint8_t SCREEN_HEIGHT = 32;
constexpr uint8_t SCREEN_ADDRESS = 0x3C;

constexpr uint8_t DHT_PIN = 4;
constexpr uint8_t DHT_TYPE = DHT11;
constexpr uint8_t SOLAR_PIN = 34;

constexpr uint32_t SOLAR_SAMPLE_INTERVAL_MS = 200;
constexpr uint32_t PUBLISH_INTERVAL_MS = 1000;
constexpr uint32_t DISPLAY_REFRESH_INTERVAL_MS = 1000;
constexpr uint32_t DHT_REFRESH_INTERVAL_MS = 3000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 3000;
constexpr uint32_t DISPLAY_REINIT_INTERVAL_MS = 5000;

constexpr size_t SMOOTHING_WINDOW = 10;
constexpr float ADC_REFERENCE_VOLTAGE = 3.3f;
constexpr float SOLAR_VOLTAGE_SCALE = 1.0f;
constexpr uint16_t ADC_MAX = 4095;

TwoWire DisplayWire = TwoWire(0);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &DisplayWire, -1);
DHT dht(DHT_PIN, DHT_TYPE);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

struct SolarSample {
  uint16_t rawAdc = 0;
  float rawVoltage = 0.0f;
  float smoothedVoltage = 0.0f;
  uint32_t sampledAtMs = 0;
};

struct ClimateReading {
  float temperatureC = NAN;
  float humidityPct = NAN;
  bool valid = false;
  uint32_t sampledAtMs = 0;
};

struct PublishAggregate {
  bool hasSamples = false;
  uint16_t latestAdc = 0;
  float latestVoltage = 0.0f;
  float latestSmoothedVoltage = 0.0f;
  float minVoltage = 0.0f;
  float maxVoltage = 0.0f;
  float meanVoltage = 0.0f;
  uint16_t sampleCount = 0;
};

float smoothingBuffer[SMOOTHING_WINDOW] = {};
size_t smoothingIndex = 0;
size_t smoothingCount = 0;
float smoothingSum = 0.0f;

SolarSample latestSample;
ClimateReading latestClimate;
PublishAggregate publishAggregate;

bool displayReady = false;
uint32_t lastSolarSampleAt = 0;
uint32_t lastPublishAt = 0;
uint32_t lastDisplayRefreshAt = 0;
uint32_t lastDhtReadAt = 0;
uint32_t lastWifiAttemptAt = 0;
uint32_t lastMqttAttemptAt = 0;
uint32_t lastDisplayInitAttemptAt = 0;

void appendSmoothedValue(float rawVoltage) {
  if (smoothingCount < SMOOTHING_WINDOW) {
    smoothingBuffer[smoothingIndex] = rawVoltage;
    smoothingSum += rawVoltage;
    ++smoothingCount;
  } else {
    smoothingSum -= smoothingBuffer[smoothingIndex];
    smoothingBuffer[smoothingIndex] = rawVoltage;
    smoothingSum += rawVoltage;
  }
  smoothingIndex = (smoothingIndex + 1) % SMOOTHING_WINDOW;
}

float currentSmoothedVoltage() {
  if (smoothingCount == 0) {
    return 0.0f;
  }
  return smoothingSum / static_cast<float>(smoothingCount);
}

String formatIsoTimestampUtc() {
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return String();
  }

  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(buffer);
}

float adcToVoltage(uint16_t rawAdc) {
  return (static_cast<float>(rawAdc) / static_cast<float>(ADC_MAX)) * ADC_REFERENCE_VOLTAGE * SOLAR_VOLTAGE_SCALE;
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptAt = millis();
  Serial.printf("Connecting to WiFi SSID=%s\n", WIFI_SSID);
}

void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  uint32_t now = millis();
  if (now - lastWifiAttemptAt < WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptAt = now;
  Serial.println("Retrying WiFi connection");
}

void setupTimeSync() {
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov", "time.google.com");
}

bool ensureDisplayReady() {
  if (displayReady) {
    return true;
  }
  uint32_t now = millis();
  if (now - lastDisplayInitAttemptAt < DISPLAY_REINIT_INTERVAL_MS) {
    return false;
  }
  lastDisplayInitAttemptAt = now;
  DisplayWire.begin(DISPLAY_SDA, DISPLAY_SCL, 100000U);
  DisplayWire.setTimeOut(50);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 init failed");
    displayReady = false;
    return false;
  }
  display.clearDisplay();
  display.display();
  displayReady = true;
  Serial.println("SSD1306 ready");
  return true;
}

void setupMqtt() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(512);
}

void ensureMqttConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }
  uint32_t now = millis();
  if (now - lastMqttAttemptAt < MQTT_RETRY_INTERVAL_MS) {
    return;
  }
  lastMqttAttemptAt = now;
  Serial.printf("Connecting MQTT %s:%d\n", MQTT_HOST, MQTT_PORT);
  bool connected = false;
  if (strlen(MQTT_USERNAME) > 0) {
    connected = mqttClient.connect(SOLAR_SENSOR_ID "-esp32", MQTT_USERNAME, MQTT_PASSWORD_AUTH);
  } else {
    connected = mqttClient.connect(SOLAR_SENSOR_ID "-esp32");
  }
  if (!connected) {
    Serial.printf("MQTT connect failed rc=%d\n", mqttClient.state());
  } else {
    Serial.println("MQTT connected");
  }
}

void readClimateIfDue() {
  uint32_t now = millis();
  if (now - lastDhtReadAt < DHT_REFRESH_INTERVAL_MS) {
    return;
  }
  lastDhtReadAt = now;

  float humidity = dht.readHumidity();
  float temperatureC = dht.readTemperature();
  if (isnan(humidity) || isnan(temperatureC)) {
    Serial.println("DHT11 read failed");
    latestClimate.valid = false;
    return;
  }

  latestClimate.temperatureC = temperatureC;
  latestClimate.humidityPct = humidity;
  latestClimate.valid = true;
  latestClimate.sampledAtMs = now;
}

void sampleSolarIfDue() {
  uint32_t now = millis();
  if (now - lastSolarSampleAt < SOLAR_SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSolarSampleAt = now;

  uint16_t rawAdc = static_cast<uint16_t>(analogRead(SOLAR_PIN));
  float rawVoltage = adcToVoltage(rawAdc);
  appendSmoothedValue(rawVoltage);
  float smoothedVoltage = currentSmoothedVoltage();

  latestSample.rawAdc = rawAdc;
  latestSample.rawVoltage = rawVoltage;
  latestSample.smoothedVoltage = smoothedVoltage;
  latestSample.sampledAtMs = now;

  if (!publishAggregate.hasSamples) {
    publishAggregate.hasSamples = true;
    publishAggregate.latestAdc = rawAdc;
    publishAggregate.latestVoltage = rawVoltage;
    publishAggregate.latestSmoothedVoltage = smoothedVoltage;
    publishAggregate.minVoltage = rawVoltage;
    publishAggregate.maxVoltage = rawVoltage;
    publishAggregate.meanVoltage = rawVoltage;
    publishAggregate.sampleCount = 1;
  } else {
    publishAggregate.latestAdc = rawAdc;
    publishAggregate.latestVoltage = rawVoltage;
    publishAggregate.latestSmoothedVoltage = smoothedVoltage;
    publishAggregate.minVoltage = min(publishAggregate.minVoltage, rawVoltage);
    publishAggregate.maxVoltage = max(publishAggregate.maxVoltage, rawVoltage);
    publishAggregate.meanVoltage =
        ((publishAggregate.meanVoltage * publishAggregate.sampleCount) + rawVoltage) /
        static_cast<float>(publishAggregate.sampleCount + 1);
    ++publishAggregate.sampleCount;
  }
}

void publishAggregateIfDue() {
  uint32_t now = millis();
  if (now - lastPublishAt < PUBLISH_INTERVAL_MS) {
    return;
  }
  lastPublishAt = now;

  if (!publishAggregate.hasSamples || !mqttClient.connected()) {
    return;
  }

  JsonDocument payload;
  payload["sensor_id"] = SOLAR_SENSOR_ID;
  String timestamp = formatIsoTimestampUtc();
  if (timestamp.length() > 0) {
    payload["timestamp"] = timestamp;
  }
  payload["adc_raw"] = publishAggregate.latestAdc;
  payload["raw_voltage"] = serialized(String(publishAggregate.latestVoltage, 6));
  payload["smoothed_voltage"] = serialized(String(publishAggregate.latestSmoothedVoltage, 6));
  payload["min_v"] = serialized(String(publishAggregate.minVoltage, 6));
  payload["max_v"] = serialized(String(publishAggregate.maxVoltage, 6));
  payload["mean_v"] = serialized(String(publishAggregate.meanVoltage, 6));
  payload["sample_count"] = publishAggregate.sampleCount;
  payload["uptime_seconds"] = millis() / 1000UL;

  if (latestClimate.valid) {
    payload["temperature_c"] = serialized(String(latestClimate.temperatureC, 1));
    payload["humidity_pct"] = serialized(String(latestClimate.humidityPct, 1));
  }

  char buffer[384];
  size_t written = serializeJson(payload, buffer, sizeof(buffer));
  bool ok = mqttClient.publish(MQTT_TOPIC, reinterpret_cast<const uint8_t*>(buffer), written, false);
  if (ok) {
    Serial.printf(
        "Published MQTT raw=%.3f smooth=%.3f min=%.3f max=%.3f samples=%u temp=%s hum=%s\n",
        publishAggregate.latestVoltage,
        publishAggregate.latestSmoothedVoltage,
        publishAggregate.minVoltage,
        publishAggregate.maxVoltage,
        publishAggregate.sampleCount,
        latestClimate.valid ? String(latestClimate.temperatureC, 1).c_str() : "--",
        latestClimate.valid ? String(latestClimate.humidityPct, 1).c_str() : "--");
  } else {
    Serial.println("MQTT publish failed");
  }

  publishAggregate = PublishAggregate{};
}

void drawDisplayIfDue() {
  uint32_t now = millis();
  if (now - lastDisplayRefreshAt < DISPLAY_REFRESH_INTERVAL_MS) {
    return;
  }
  lastDisplayRefreshAt = now;

  if (!ensureDisplayReady()) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("V:");
  display.print(latestSample.rawVoltage, 3);
  display.print(" S:");
  display.print(latestSample.smoothedVoltage, 3);

  display.setCursor(0, 10);
  display.print("A34:");
  display.print(latestSample.rawAdc);
  display.print(" WiFi:");
  display.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");

  display.setCursor(0, 20);
  display.print("M:");
  display.print(mqttClient.connected() ? "OK" : "NO");
  display.print(" T:");
  if (latestClimate.valid) {
    display.print(latestClimate.temperatureC, 1);
    display.print("C");
  } else {
    display.print("--.-");
  }

  display.setCursor(78, 20);
  display.print("H:");
  if (latestClimate.valid) {
    display.print(latestClimate.humidityPct, 0);
    display.print("%");
  } else {
    display.print("--");
  }

  display.display();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  analogReadResolution(12);
  analogSetPinAttenuation(SOLAR_PIN, ADC_11db);

  ensureDisplayReady();
  if (displayReady) {
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("ESP32 solar node");
    display.println("Booting...");
    display.display();
  }

  dht.begin();
  setupWifi();
  setupTimeSync();
  setupMqtt();
}

void loop() {
  ensureWifiConnected();
  ensureMqttConnected();
  sampleSolarIfDue();
  readClimateIfDue();
  publishAggregateIfDue();
  drawDisplayIfDue();
  mqttClient.loop();
  delay(10);
}
