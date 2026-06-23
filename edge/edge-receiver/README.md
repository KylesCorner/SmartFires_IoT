# SmartFires Edge Receiver

Refactored Jetson-side base station ingest service.

### Install

```bash
pip install -e .
```

### Run Receiver

```bash
smartfires-edge receive \
  --port /dev/smartfires-base \
  --baud 115200 \
  --data-dir /mnt/nvme_drive/data \
  --sync-interval 600
```

### Enable Jetson Anemometer Logging

```bash
smartfires-edge receive \
  --port /dev/smartfires-base \
  --anemometer-port /dev/ttyUSB0 \
  --anemometer-baud 9600 \
  --anemometer-address 1 \
  --anemometer-interval 1.0
```

When enabled, each telemetry row includes:

- `jetson_wind_mps`
- `jetson_wind_dir_deg`

STATUS packets are also written to the telemetry CSV (in addition to `status/*.jsonl`).
Those rows use `packet_type=status` and populate:

- `gps_valid`
- `battery_valid`
- `battery_mv`
- `battery_pct`

Standard-packet app-layer reliability is owned by the base station firmware.
The Jetson receiver only ingests forwarded LoRa traffic and sends `TIME_SYNC`.

### Packet Loss Summary

```bash
smartfires-edge summary --data-dir /mnt/nvme_drive/data
```

### Live Visualizer

```bash
smartfires-edge visualize \
  --port /dev/smartfires-base \
  --baud 115200 \
  --sync-interval 600 \
  --telemetry-rows 20
```

`visualize` renders two live terminal tables:

- telemetry samples (sample timestamp + sensor readings)
- battery/location status (GPS validity, lat/lon, battery)

### Web Dashboard

```bash
smartfires-edge web \
  --port /dev/smartfires-base \
  --baud 115200 \
  --host 0.0.0.0 \
  --http-port 8000
```

Runs the same UART ingest (and, if `--anemometer-port` is given, the anemometer poller) on a
background thread, and serves a FastAPI/uvicorn dashboard on the main thread: a live map, a
sniffer view, a debug log view, and a map-history view, backed by a REST API and WebSocket
streams. Pass `--sniffer-port` (plus optionally `--sniffer-baud` / `--num-slots`) to also feed
the dashboard's sniffer tab from a passive LoRa sniffer Feather; the sniffer tab stays disabled
otherwise.

There is currently no way to send `CMD_CALIBRATE`/`CMD_RESET` to a node from here — the
dashboard's `/api/command` endpoint is a stub that echoes the request without writing to serial.
Calibration and reset commands can only be observed passively today, via the sniffer feed
decoding `CMD_CALIBRATE`/`CMD_RESET`/`CMD_ACK` traffic for monitoring.

### Heading and Calibration

There is no Jetson-side calibration data exchange (`PKT_CALIBRATION_DATA` is reserved and
unimplemented — no encode/decode exists for it). Heading comes entirely from the node's
on-chip DMP 9-DOF fusion, which self-calibrates via figure-8 motion at boot; the Jetson just
reads `heading_deg_x10`/`heading_accuracy` out of each STATUS packet and applies magnetic
declination correction (via the `geomag` package, given a GPS fix) in `session.py`.
