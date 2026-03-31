# Pringles-Meteorological-Station

![Status](https://img.shields.io/badge/status-active%20prototype-58ffa9?style=flat-square)

Pringles-Meteorological-Station is an edge-to-cloud weather and analytics station built inside a Pringles can.

It reads light and climate data at the edge, ships telemetry over MQTT, stores it in a time-series database, serves live and historical views through FastAPI, and runs an online ML layer that tries to infer solar time from the physics of light instead of cheating with the system clock.

This is a real system, not a mockup. It just happens to live in snack packaging.

## Why This Exists

The project started as a practical experiment:

- can a tiny improvised weather node survive as a long-running edge device
- can it keep a clean data pipeline from sensor to UI
- can the ML layer learn something physically meaningful from noisy light telemetry

The result is a compact, opinionated station with a deliberately over-engineered backend and a deliberately under-glamorous enclosure.

## System Architecture

Text version of the pipeline:

```text
Solar Panel + DHT11
        ->
      ESP32
        ->
       MQTT
        ->
  Ingestion Worker
        ->
     InfluxDB
        ->
      FastAPI
        ->
   SSE / JSON API
        ->
Vanilla JS + Chart.js UI
```

Runtime responsibilities:

- the ESP32 reads edge sensors, updates the OLED, and publishes aggregated telemetry
- MQTT acts as the transport layer between the edge node and the server
- the ingestion worker validates and persists telemetry into InfluxDB
- FastAPI exposes history, live data, climate summaries, and ML insights
- the frontend renders the public-facing dashboard
- `ml_engine` retrains periodically on recent telemetry and refreshes the current model snapshot

## Hardware Stack

Current edge hardware:

- `ESP32` as the main edge controller
- a small solar panel wired directly into the ESP32 ADC path on `GPIO34`
- `DHT11` for temperature and humidity
- `0.96"` OLED over I2C for local monitoring
- hardwired copper connections instead of fragile Dupont wiring

Current server-side stack:

- `Mosquitto` for MQTT
- `InfluxDB` for time-series storage
- Python ingestion and ML workers
- `FastAPI` for the backend API
- `Nginx` as the static frontend container
- Cloudflare Tunnel for public exposure, for example on `weather.datanode.live`

## Hardware Journey

The first version was built around a Raspberry Pi Zero 2W and a breadboard.

That phase included:

- long soldering sessions
- breadboard power drama
- voltage drops down to roughly `1.6V`
- one fairly educational short circuit

After about 13 hours of what can politely be called hardware character building, the architecture was revised.

The current edge node runs on ESP32 because it is:

- colder
- simpler
- more electrically stable
- much harder to accidentally bully into undefined behavior

In short, the Raspberry Pi prototype paid the Ohms of despair so the ESP32 could live a quieter life.

## Software and Data Pipeline

At a high level:

- the ESP32 samples the sensors at `1 Hz`
- it publishes aggregated MQTT packets every `5 seconds`
- the home server ingests those packets through a dedicated worker
- the worker writes canonical telemetry into InfluxDB
- FastAPI serves live and historical data over JSON plus `SSE`
- the frontend is a single-page application built with Vanilla JS and `Chart.js`

Core software choices:

- edge firmware: Arduino-style C++ on ESP32
- messaging: `MQTT`
- storage: `InfluxDB`
- API: `FastAPI`
- frontend: Vanilla JS + `Chart.js`
- deployment: `docker-compose`

## Machine Learning Layer

The ML layer lives in the `ml_engine` container and retrains online on recent telemetry.

Its current job is not to forecast the stock market or discover consciousness. It does three more grounded things:

1. classify the current phase of the day
2. estimate a light-derived solar clock
3. estimate time to sunrise or sunset when the current phase makes that meaningful

### What The Models See

The models work on engineered light features such as:

- raw voltage
- smoothed voltage
- short-window and medium-window deltas
- rolling statistics
- voltage relative to the running daily maximum

### Noise Handling

The solar signal is noisy, especially at low light and during transitions. To keep it usable, the pipeline relies on:

- aggregated edge packets instead of pure raw spam
- smoothed reference values
- rolling statistics
- day-phase gating
- anomaly detection for physically suspicious behavior

### No Data Leakage

The solar clock does **not** use system time as an input feature.

That was an explicit fix after earlier iterations risked learning the clock from the answer key instead of from the signal. The current setup predicts local solar time from light behavior only.

## How To Run

Start the server stack:

```bash
cd /home/tim/projects/sollar_panel
cp server/.env.example server/.env
docker compose --env-file server/.env -f server/docker-compose.yml up --build -d
docker compose --env-file server/.env -f server/docker-compose.yml ps
```

Default local ports:

- MQTT broker: `1884`
- InfluxDB: `127.0.0.1:18086`
- FastAPI: `127.0.0.1:18000`
- frontend: `127.0.0.1:13000`

The edge firmware lives here:

```bash
cd /home/tim/projects/sollar_panel/edge/esp32
cp include/secrets.example.h include/secrets.h
```

Then fill in:

- Wi-Fi SSID
- Wi-Fi password
- MQTT host
- MQTT port

Build and flash from your development machine with PlatformIO.

## Repository Map

- `edge/esp32/` — ESP32 firmware
- `server/backend/` — FastAPI API layer
- `server/worker/` — MQTT ingestion worker
- `server/ml_engine/` — online training and model snapshot writer
- `server/frontend/` — static frontend and Nginx config
- `server/docker-compose.yml` — full server stack definition
- `docs/` — supporting architecture and implementation notes
- `AGENT.md` — repository workflow rules

## Documentation

- [Project overview](docs/project-overview.md)
- [Edge device pipeline](docs/edge-device-pipeline.md)
- [Server pipeline](docs/server-pipeline.md)
- [Cloudflare Tunnel integration](docs/cloudflare-tunnel.md)
- [Repository operating rules](AGENT.md)

## Contributing

This repository follows a documentation-first workflow.

Before changing behavior:

1. update the relevant docs when architecture or behavior changes
2. update `README.md` when onboarding or system understanding changes
3. update `AGENT.md` if the workflow rules change
4. create a local commit for each completed change set
5. do not push unless explicitly requested

## License

No license has been declared yet.
