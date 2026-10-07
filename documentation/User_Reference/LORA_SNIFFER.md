---
name: lora-sniffer
description: Flashing and operating the passive LoRa sniffer with the Jetson dashboard.
category: reference
status: current
last_verified: 2026-10-07
source_refs:
  - platformio/platformio.ini
  - platformio/src/main_lora_sniffer.cpp
  - edge/edge-receiver/src/smartfires_edge/sniffer_service.py
  - edge/edge-receiver/src/smartfires_edge/web/static/js/sniffer_page.js
related_docs:
  - jetson-cheatsheet
  - tdma-protocol
---

# LoRa sniffer

The `feather_m0_lora_sniffer` target passively receives SmartFires LoRa traffic and writes one NDJSON record per event over native USB at 115200 baud. It compiles the same `SMARTFIRES_NETWORK_PROFILE` as the base and nodes, explicitly applies that modem tuple, and announces its profile ID/fingerprint and TDMA geometry in the startup `config` event. It does not join, transmit, assign nodes, or replace the base.

## Flash and inspect

From `SmartFires_IoT/platformio` with the sniffer Feather connected and a 915 MHz antenna attached:

```bash
pio run -e feather_m0_lora_sniffer --target upload
pio device monitor -e feather_m0_lora_sniffer
```

Stop the monitor before starting the dashboard; only one process can own the serial port.

## Stable device name

Create `/dev/smartfires-sniffer` with the udev procedure in `JETSON_CHEATSHEET.md`. The base and sniffer enumerate similarly, so raw `/dev/ttyACM0`/`ttyACM1` names are not stable across reconnects.

## Dashboard

```bash
smartfires-edge web \
  --port /dev/smartfires-base \
  --sniffer-port /dev/smartfires-sniffer \
  --sniffer-baud 115200 \
  --num-slots 5 \
  --http-port 8080
```

Open `http://<jetson-ip>:8080/sniffer`. The sniffer thread parses NDJSON and derives packet counts, RSSI/SNR, TDMA slot position, timing jitter, and guard-window violations.

The sniffer is optional: a missing or failed sniffer reports an error without
taking down core base-station ingest. During a graceful **New Session** restart,
the worker observes the shared shutdown event, releases its serial handle, and
is recreated with a fresh timing anchor in the replacement process.

The dashboard normally takes slot count, width, and guards from the base-announced profile. `--num-slots` remains a pre-announcement/legacy fallback. The sniffer's own fingerprint is compared with the active base/override profile; disagreement appears as a profile mismatch and its timing statistics should not be trusted until the fleet is rebuilt consistently.

## Expected limitations

- A passive receiver can miss frames because of RF conditions, USB interruption, or its own startup; absence in sniffer output is not proof that a transmitter did not send.
- The sniffer observes over-the-air packets but cannot see USB-only `PKT_DEBUG_LOG` frames.
- The sniffer cannot observe USB-only `PKT_NETWORK_PROFILE`; it reports its independently compiled identity through NDJSON instead.
- It does not decrypt or reassemble another protocol; it understands the SmartFires binary header and known packet types.
- It must use the same frequency and RadioHead modem settings as the deployment.
- RSSI/SNR are measurements at the sniffer location, not at the base.

For end-to-end validation, compare sniffer events with the base/Jetson ingest and node/base structured logs rather than using any one stream alone.
