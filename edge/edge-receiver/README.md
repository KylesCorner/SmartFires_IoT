# SmartFires Edge Receiver

Refactored Jetson-side base station ingest service.

### Install

```bash
pip install -e .
```

### Run Receiver

```bash
smartfires-edge receive \
  --port /dev/ttyTHS1 \
  --baud 115200 \
  --data-dir /mnt/nvme_drive/data \
  --sync-interval 600
```

### Enable Jetson Anemometer Logging

```bash
smartfires-edge receive \
  --port /dev/ttyTHS1 \
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
  --port /dev/ttyTHS1 \
  --baud 115200 \
  --sync-interval 600 \
  --telemetry-rows 20
```

`visualize` renders two live terminal tables:

- telemetry samples (sample timestamp + sensor readings)
- battery/location status (GPS validity, lat/lon, battery)

### Interactive Jetson CLI

Use the interactive CLI to watch live packets, send node commands, and manage
session calibration data.

```bash
smartfires-edge cli \
  --port /dev/ttyTHS1 \
  --baud 115200
```

Optional session file override:

```bash
smartfires-edge cli \
  --port /dev/ttyTHS1 \
  --baud 115200 \
  --session-file ~/.smartfires/session.json
```

The CLI uses a split terminal layout:

- top area: live packet and command log
- bottom area: command prompt

Type `help` in the CLI to list commands. Supported commands:

- `calibrate node <id>`
- `cal <id>`
- `reset node <id>`
- `reset node <id> hard`
- `list nodes`
- `list calibrations`
- `save session`
- `load session`
- `clear calibration <id>`
- `clear calibrations`
- `help [command]`
- `quit` or `exit`

Behavior notes:

- after `calibrate` or `reset`, the CLI warns if no `CMD_ACK` arrives after 5 seconds
- after `calibrate`, the CLI warns if no `CALIBRATION_DATA` arrives after
  `(duration_s + 15)` seconds
- AWAKEN packets update node_id <-> uid_hash mappings in session storage
- session state is saved on graceful CLI exit

## Phase 6 E2E Test Checklist (Pass/Fail)

Use this checklist to execute integration testing in a repeatable order.

### Preconditions

- base station firmware running with Phase 0-5 changes
- node firmware running with Phase 0-5 changes
- receiver host has serial access to base station (`/dev/ttyTHS1` or equivalent)
- clear terminal for node serial monitor and base serial monitor
- known test location and reference compass available for heading spot-check

### Start CLI and Capture Logs

Run:

```bash
smartfires-edge cli --port /dev/ttyTHS1 --baud 115200
```

In a second shell, capture CLI output to a timestamped file:

```bash
mkdir -p /tmp/smartfires-test
script /tmp/smartfires-test/cli-$(date +%Y%m%d-%H%M%S).log
```

### Scenario Execution Order

1. Cold start (no calibration)
2. Calibration flow (node 1)
3. Heading accuracy spot-check (node 1)
4. Multi-node calibration (nodes 1 and 2)
5. Reset command behavior (node 2)
6. Session persistence (restart CLI process)
7. CLI responsiveness under traffic
8. Quality rejection (intentionally poor rotation)

### Pass/Fail Criteria by Scenario

1. Cold start
  PASS when AWAKEN appears, `list nodes` shows node present, and heading is `--` before calibration.
2. Calibration flow
  PASS when `calibrate node 1` yields CMD_ACK, then CALIBRATION_DATA, then heading appears on next STATUS.
3. Heading accuracy
  PASS when physically rotating node changes heading in expected direction and rough magnitude.
4. Multi-node
  PASS when both nodes can be calibrated and both show heading updates.
5. Reset
  PASS when `reset node 2` yields CMD_ACK, node reboots/AWAKEN reappears, and heading resumes from saved calibration.
6. Session persistence
  PASS when exiting/restarting CLI preserves calibration and heading recomputes without recalibration.
7. CLI responsiveness
  PASS when commands return promptly and packet log continues without obvious stalls or drops.
8. Quality rejection
  PASS when poor rotation calibration attempt produces warning/rejection for axis range or covariance quality.

### Operator Command Sequence

Run these inside the CLI in order:

```text
list nodes
calibrate node 1
list calibrations
list nodes
calibrate node 2
list calibrations
reset node 2
save session
quit
```

Restart CLI and verify persistence:

```text
list calibrations
list nodes
```

### Test Evidence Template

Record one row per scenario:

| Scenario | Start Time | End Time | Result (PASS/FAIL) | Evidence File | Notes |
| --- | --- | --- | --- | --- | --- |
| Cold start | | | | | |
| Calibration flow node 1 | | | | | |
| Heading accuracy node 1 | | | | | |
| Multi-node | | | | | |
| Reset node 2 | | | | | |
| Session persistence | | | | | |
| CLI responsiveness | | | | | |
| Quality rejection | | | | | |
