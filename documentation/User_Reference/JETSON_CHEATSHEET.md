---
name: jetson-cheatsheet
description: Common Jetson-side commands — installing edge-receiver, pulling data, the web dashboard, and one-time udev setup for stable base/sniffer device paths.
category: reference
status: current
last_verified: 2026-09-04
source_refs:
  - util/udev/99-smartfires.rules
  - edge/edge-receiver/src/smartfires_edge/main.py
related_docs:
  - jetson-bridge
---

# SmartFires Jetson cheatsheet

## Install or update

From the repository root:

```bash
python3 -m pip install --use-pep517 -e edge/edge-receiver
```

This installs `smartfires-edge` with `receive`, `summary`, `visualize`, and `web` subcommands.

## Run

```bash
# Durable ingest only
smartfires-edge receive --port /dev/smartfires-base \
  --data-dir /mnt/nvme_drive/data

# Live terminal tables
smartfires-edge visualize --port /dev/smartfires-base

# Dashboard plus ingest (default bind 0.0.0.0:8080)
smartfires-edge web --port /dev/smartfires-base

# Dashboard with passive sniffer
smartfires-edge web --port /dev/smartfires-base \
  --sniffer-port /dev/smartfires-sniffer --num-slots 5

# Saved loss summary
smartfires-edge summary --data-dir /mnt/nvme_drive/data
```

Open `http://<jetson-ip>:8080`. `--num-slots` affects only sniffer alignment and must match firmware `NUM_SLOTS`; omit it to use the edge default.

The dashboard can issue a real per-node reset and DYNAMIC/STATIC TX-power commands. The generic `/api/command` endpoint remains an echo stub, and there is no calibration CLI.

## Optional Jetson anemometer

Add these arguments to `receive` or `web`:

```bash
--anemometer-port /dev/ttyUSB0 --anemometer-baud 9600 \
--anemometer-address 1 --anemometer-interval 1.0
```

## Stable USB device names

The base and sniffer Feathers have the same USB VID/PID, so `/dev/ttyACM*` ordering can swap. With only one board attached, identify its serial:

```bash
udevadm info -a -n /dev/ttyACM0 | grep '{serial}'
```

Install one rule per board in `/etc/udev/rules.d/99-smartfires.rules` (the repo template is `util/udev/99-smartfires.rules`):

```text
SUBSYSTEM=="tty", ATTRS{serial}=="<base-serial>", SYMLINK+="smartfires-base"
SUBSYSTEM=="tty", ATTRS{serial}=="<sniffer-serial>", SYMLINK+="smartfires-sniffer"
```

Reload and check:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
ls -l /dev/smartfires-base /dev/smartfires-sniffer
```

## Service manager

From the repo root:

```bash
./edge/smartfires-manager.sh status
./edge/smartfires-manager.sh update-edge
./edge/smartfires-manager.sh deploy
```

Read `SMARTFIRES_MANAGER.md` before using flash/deploy actions.

## Data transfer

```bash
rsync -avz --progress \
  smartfires@10.8.184.94:/mnt/nvme_drive/data/ ./data/
```

Adjust the host as needed; `util/rsync_from_jetson.sh` contains the saved project variant.

## Networking and diagnostics

```bash
hostname -I
sudo nmcli connection up wired-dhcp
systemctl --no-pager --full status smartfires-edge.service
journalctl -u smartfires-edge.service -f
```

Do not run `smartfires-edge`, a serial monitor, and the systemd receiver against `/dev/smartfires-base` at the same time. Stop the current owner first.
