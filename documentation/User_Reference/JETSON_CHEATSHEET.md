---
name: jetson-cheatsheet
description: Common Jetson-side commands — installing edge-receiver, pulling data, the web dashboard, and one-time udev setup for stable base/sniffer device paths.
category: reference
status: current
last_verified: 2026-06-23
source_refs:
  - util/udev/99-smartfires.rules
  - edge/edge-receiver/src/smartfires_edge/main.py
related_docs:
  - uart-jetson-bridge
---

# SmartFires — Jetson Cheatsheet

## Install the edge-receiver package

From the repo root on the Jetson:

```bash
python3 -m pip install --use-pep517 -e .
```

or equivalently, from anywhere:

```bash
pip install -e edge/edge-receiver
```

This installs the `smartfires-edge` console script (defined in
`edge/edge-receiver/pyproject.toml`'s `[project.scripts]`), exposing the
`receive`, `summary`, `visualize`, and `web` subcommands used below.

## Pull data off the Jetson

```bash
rsync -avz --progress smartfires@10.8.184.94:/mnt/nvme_drive/data/ ./data/
```

(See also `util/rsync_from_jetson.sh` for a saved variant of this command.)

## Bring up wired Ethernet (if DHCP didn't apply automatically)

```bash
sudo nmcli connection up wired-dhcp
```

## Web dashboard

`smartfires-edge web` runs UART ingest (CSV logging, packet-loss tracking) and
the live web dashboard (Signal Map, Link Quality, Reception Timeline) in one
process — no separate `receive` step needed.

1. On the Jetson, find its LAN IP and start the dashboard:

   ```bash
   hostname -I && smartfires-edge web --port /dev/smartfires-base
   ```

2. From any machine on the same network, open `http://<jetson-ip>:8080` in a browser.

Defaults: host `0.0.0.0` (LAN-reachable), port `8080`. Override with `--host`/`--http-port`.
CSV + metrics still land under the default data dir unless `--data-dir` is set.

The base station and sniffer are both USB-connected Feathers now and enumerate
identically (same VID/PID), so raw `/dev/ttyACM*` paths can swap between them
on reboot/reconnect. Use the udev-assigned stable symlinks instead — see
"One-time udev setup" below.

## One-time udev setup

Run once per board, with only that board plugged in, to find its USB serial
number:

```bash
udevadm info -a -n /dev/ttyACM0 | grep '{serial}'
```

Add the result to `/etc/udev/rules.d/99-smartfires.rules`, one line per board:

```text
SUBSYSTEM=="tty", ATTRS{serial}=="<base-serial>", SYMLINK+="smartfires-base"
SUBSYSTEM=="tty", ATTRS{serial}=="<sniffer-serial>", SYMLINK+="smartfires-sniffer"
```

then `sudo udevadm control --reload-rules && sudo udevadm trigger`.

Current working web command: (Do not delete)
smartfires-edge web --port /dev/smartfires-base --sniffer-port /dev/smartfires-sniffer --num-slots 4
