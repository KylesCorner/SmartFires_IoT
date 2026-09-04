#!/usr/bin/env bash
set -euo pipefail

PORT="${1:-/dev/smartfires-base}"
DATA_DIR="${2:-/mnt/nvme_drive/data}"

mkdir -p "$DATA_DIR"

smartfires-edge receive \
    --port "$PORT" \
    --data-dir "$DATA_DIR"
