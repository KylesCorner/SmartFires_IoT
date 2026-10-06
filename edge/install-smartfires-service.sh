#!/usr/bin/env bash
set -euo pipefail

# Install the repository-owned service template. Run as root (or via sudo)
# after installing the package into the selected virtual environment.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TEMPLATE="$SCRIPT_DIR/smartfires-edge.service.in"
DEST="/etc/systemd/system/smartfires-edge.service"

: "${SMARTFIRES_USER:?Set SMARTFIRES_USER to the unprivileged service account}"
SMARTFIRES_GROUP="${SMARTFIRES_GROUP:-$SMARTFIRES_USER}"
: "${SMARTFIRES_VENV:?Set SMARTFIRES_VENV to the installed virtual environment}"
SMARTFIRES_DATA_DIR="${SMARTFIRES_DATA_DIR:-/mnt/nvme_drive/data}"
SMARTFIRES_DATA_MOUNT="${SMARTFIRES_DATA_MOUNT:-/mnt/nvme_drive}"
SMARTFIRES_INSTALL_ROOT="${SMARTFIRES_INSTALL_ROOT:-/opt/smartfires/SmartFires_IoT}"

[[ -r "$TEMPLATE" ]] || { echo "Missing service template: $TEMPLATE" >&2; exit 1; }
[[ -x "$SMARTFIRES_VENV/bin/smartfires-edge" ]] || {
    echo "Missing executable: $SMARTFIRES_VENV/bin/smartfires-edge" >&2
    exit 1
}
[[ "$SMARTFIRES_DATA_DIR" == "$SMARTFIRES_DATA_MOUNT"/* ]] || {
    echo "SMARTFIRES_DATA_DIR must be below SMARTFIRES_DATA_MOUNT" >&2
    exit 1
}

rendered="$(sed \
    -e "s|@SMARTFIRES_USER@|$SMARTFIRES_USER|g" \
    -e "s|@SMARTFIRES_GROUP@|$SMARTFIRES_GROUP|g" \
    -e "s|@SMARTFIRES_VENV@|$SMARTFIRES_VENV|g" \
    -e "s|@SMARTFIRES_DATA_DIR@|$SMARTFIRES_DATA_DIR|g" \
    -e "s|@SMARTFIRES_DATA_MOUNT@|$SMARTFIRES_DATA_MOUNT|g" \
    -e "s|@SMARTFIRES_INSTALL_ROOT@|$SMARTFIRES_INSTALL_ROOT|g" \
    "$TEMPLATE")"

install -D -m 0644 /dev/stdin "$DEST" <<<"$rendered"
systemctl daemon-reload
systemctl enable smartfires-edge.service
echo "Installed and enabled $DEST"
echo "Start after confirming the data mount: systemctl start smartfires-edge.service"
