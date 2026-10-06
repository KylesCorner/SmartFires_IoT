#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/smartfires-base}"
DATA_DIR="${2:-/mnt/nvme_drive/data}"

# Compatibility entrypoint retained for older field notes. The dashboard is
# now the deployment contract, and the wrapper supplies New Session recovery.
exec "$(dirname -- "$0")/run_web_forever.sh" "$PORT" "$DATA_DIR"
