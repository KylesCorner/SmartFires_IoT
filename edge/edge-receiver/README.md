## SmartFires Edge Receiver

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
	--sync-interval 600 \
	--ack-interval 4.0
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

### Packet Loss Summary

```bash
smartfires-edge summary --data-dir /mnt/nvme_drive/data
```
