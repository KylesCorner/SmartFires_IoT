#!/usr/bin/env bash
set -Eeuo pipefail

# One-command Jetson software update. Optional manager flags such as
# `--branch NAME` may be supplied before the implicit update-edge command.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

exec "$SCRIPT_DIR/smartfires-manager.sh" "$@" update-edge
