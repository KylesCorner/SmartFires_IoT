# SmartFires edge receiver

Installable Jetson-side package for USB ingest, durable telemetry, live visualization, and the FastAPI dashboard.

## Install

From this directory:

```bash
python3 -m pip install --use-pep517 -e .
```

From the repository root, use `-e edge/edge-receiver` instead.

## Commands

```bash
smartfires-edge receive \
  --port /dev/smartfires-base \
  --baud 115200 \
  --data-dir /mnt/nvme_drive/data \
  --sync-interval 600

smartfires-edge summary --data-dir /mnt/nvme_drive/data

smartfires-edge visualize \
  --port /dev/smartfires-base \
  --baud 115200 \
  --telemetry-rows 20

smartfires-edge web \
  --port /dev/smartfires-base \
  --host 0.0.0.0 \
  --http-port 8080
```

`web` runs UART ingest in a background thread and serves the live map, sniffer, debug-log, live-log, and map-history views. Add `--sniffer-port /dev/smartfires-sniffer --num-slots 5` to enable the passive-sniffer view.

To merge the optional Jetson-connected ES-W302 readings into telemetry rows, add:

```bash
--anemometer-port /dev/ttyUSB0 --anemometer-baud 9600 \
--anemometer-address 1 --anemometer-interval 1.0
```

## Responsibilities

- Parses the base's framed USB stream and mirrors the C++ packet schema.
- Expands bundles, writes telemetry/status records, maintains session metadata and packet-loss metrics, and serves live state.
- Sends clock authority to the base at `--sync-interval`; the base rebroadcasts cached session time more frequently over LoRa.
- Starts a fresh session by issuing a base soft reset through the USB command path.
- Sends real node reset commands through the dashboard's `/api/node_reset` endpoint.
- Controls per-node dynamic/static TX power through `/api/tx_power`.

The general `/api/command` route is still an echo stub. There is no implemented calibration workflow: `CMD_CALIBRATE` can traverse the protocol, but node behavior is deliberately log-and-ACK because the ICM-20948 DMP self-calibrates.

STATUS frames are included in telemetry output and contain GPS/battery validity, DMP heading and accuracy, lifetime retransmit/failure counters, and applied TX power. `session.py` can apply magnetic-declination correction when a valid GPS fix is available.

See [`../../documentation/Current_Architecture/JETSON_BRIDGE.md`](../../documentation/Current_Architecture/JETSON_BRIDGE.md) for the framing contract and [`../../documentation/User_Reference/JETSON_CHEATSHEET.md`](../../documentation/User_Reference/JETSON_CHEATSHEET.md) for deployment setup.
