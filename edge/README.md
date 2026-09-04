# Edge unit

The Jetson package lives in [`edge-receiver/`](edge-receiver/) and supplies `receive`, `summary`, `visualize`, and `web` subcommands. Install it from the repository root with:

```bash
python3 -m pip install --use-pep517 -e edge/edge-receiver
```

Use `/dev/smartfires-base` for the udev-managed base connection. [`smartfires-manager.sh`](smartfires-manager.sh) manages the receiver service; [`anemometer_read.py`](anemometer_read.py) remains available as a standalone ES-W302 check.
