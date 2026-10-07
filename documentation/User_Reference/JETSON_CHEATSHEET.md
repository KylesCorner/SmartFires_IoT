---
name: jetson-cheatsheet
description: Common Jetson-side commands — installing edge-receiver, pulling data, the web dashboard, and one-time udev setup for stable base/sniffer device paths.
category: reference
status: current
last_verified: 2026-10-07
source_refs:
  - util/udev/99-smartfires.rules
  - edge/edge-receiver/src/smartfires_edge/main.py
  - edge/smartfires-edge.service.in
  - edge/install-smartfires-service.sh
  - edge/run_web_forever.sh
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

Open `http://<jetson-ip>:8080`. The base-announced network profile normally supplies sniffer slot geometry. `--num-slots` is only a recovery fallback before that announcement arrives; omit it to use the edge default. `receive` and `web` also accept `--network-profile-override PATH` for recovery-only JSON profile metadata, and any disagreement with the observed base remains visible as a mismatch.

The dashboard can issue a real per-node reset and DYNAMIC/STATIC TX-power commands. The generic `/api/command` endpoint remains an echo stub, and there is no calibration CLI.

When launched by `smartfires-edge.service`, the shared dashboard header also shows **Update Jetson**. After confirmation it fast-forwards the current configured branch, reinstalls the edge package, and gracefully restarts into a new session. The button is disabled for direct/manual web launches because those lack a guaranteed restart supervisor. Update failures leave the current process running and are shown in the browser.

The dashboard has no application login. Confirmation protects against accidental clicks, not unauthorized clients; expose port 8080 only on the trusted operations network. Use the interactive `./edge/update-jetson.sh` when the repository-owned systemd unit itself also needs to be reinstalled.

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
./edge/update-jetson.sh
./edge/smartfires-manager.sh deploy
```

`update-jetson.sh` fast-forwards the selected Git branch, preserves untracked runtime files, reinstalls the edge package and repository-owned systemd unit, and restarts/verifies the service. It refuses tracked local modifications and may prompt for the Jetson user's sudo password.

Read `SMARTFIRES_MANAGER.md` before using flash/deploy actions.

### Boot service and restart recovery

The repository-owned unit template and installer are under `edge/`. Install the
package into a venv first, then render the unit with the actual unprivileged
account and NVMe mount (the defaults below match the edge configuration):

```bash
sudo SMARTFIRES_USER=smartfires \
  SMARTFIRES_VENV=/home/smartfires/.smartfires_venv \
  SMARTFIRES_DATA_DIR=/mnt/nvme_drive/data \
  SMARTFIRES_DATA_MOUNT=/mnt/nvme_drive \
  SMARTFIRES_INSTALL_ROOT=/opt/smartfires/SmartFires_IoT \
  ./edge/install-smartfires-service.sh
sudo systemctl start smartfires-edge.service
```

The unit runs `smartfires-edge web`, waits for the data mount, and retries both
failure and intentional graceful application exits with bounded backoff. It is
enabled for boot by the installer. Check `journalctl -u smartfires-edge.service
-b` after boot; a missing mount is a visible pre-start failure and must not be
worked around by creating `/mnt/nvme_drive/data` on the root filesystem.

Keep `/dev/smartfires-base` and `/dev/smartfires-sniffer` as udev-managed paths.
The core base receiver retries delayed USB enumeration; optional sniffer and
anemometer paths may be absent without blocking core ingest. Do not run a second
receiver against the base port while the service owns it.

For a non-systemd session, `./edge/run_web_forever.sh` provides equivalent
restart-on-exit behavior with bounded backoff and the same default-mount guard;
Ctrl-C/TERM stops it. A direct `smartfires-edge web` command is a one-process
launch and requires manual restart after an intentional graceful New Session
exit.

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
