# Edge unit

The Jetson package lives in [`edge-receiver/`](edge-receiver/) and supplies `receive`, `summary`, `visualize`, and `web` subcommands. Install it from the repository root with:

```bash
python3 -m pip install --use-pep517 -e edge/edge-receiver
```

Use `/dev/smartfires-base` for the udev-managed base connection. [`smartfires-manager.sh`](smartfires-manager.sh) manages the receiver service; [`anemometer_read.py`](anemometer_read.py) remains available as a standalone ES-W302 check.

Under the systemd-managed launch, the dashboard's **Update Jetson** control runs the manager's fixed rootless `dashboard-update` subset, then requests a graceful service restart. It cannot reinstall the systemd unit; use [`update-jetson.sh`](update-jetson.sh) interactively for the full Git/package/unit/restart workflow. Keep the unauthenticated dashboard restricted to the trusted operations network.

## Service installation

[`smartfires-edge.service.in`](smartfires-edge.service.in) is the checked-in
systemd template. It runs the installed venv's `smartfires-edge web` entrypoint,
waits for the configured data mount, and uses `Restart=always` with bounded
backoff so both failure exits and the intentional graceful New Session exit are
started again. An explicit `systemctl stop` remains stopped. Render and install
it on the Jetson after setting the service account and paths:

```bash
sudo SMARTFIRES_USER=smartfires \
  SMARTFIRES_VENV=/home/smartfires/.smartfires_venv \
  SMARTFIRES_DATA_DIR=/mnt/nvme_drive/data \
  SMARTFIRES_DATA_MOUNT=/mnt/nvme_drive \
  SMARTFIRES_INSTALL_ROOT=/opt/smartfires/SmartFires_IoT \
  ./edge/install-smartfires-service.sh
sudo systemctl start smartfires-edge.service
```

The installer refuses a missing package executable or a data directory outside
the configured mount. The unit checks the mount before starting, so it cannot
silently create a same-named directory on the root filesystem. Inspect failures
with `journalctl -u smartfires-edge.service -b`.

Keep the base symlink supplied by `util/udev/99-smartfires.rules`; do not replace
it with a guessed `/dev/ttyACM*` path. Delayed USB enumeration is handled by the
application's reconnect loop. The optional sniffer and anemometer are not
required for core ingest and should be added as command-line options only when
their stable device paths exist.

For a manual launch, use [`run_web_forever.sh`](run_web_forever.sh), which
retries an application exit with a bounded 2–30 second backoff, verifies the
default data mount, and treats Ctrl-C/TERM as an explicit stop. A direct
`smartfires-edge web ...` launch has no supervisor: its process must be
restarted by the operator after a graceful New Session exit.
