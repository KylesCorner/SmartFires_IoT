#!/usr/bin/env bash
set -euo pipefail

# Small manual-launch supervisor. Ctrl-C/TERM is an explicit stop; an
# application exit is treated like systemd Restart=always and retried.
PORT="${1:-/dev/smartfires-base}"
DATA_DIR="${2:-/mnt/nvme_drive/data}"
shift $(( $# >= 2 ? 2 : $# ))
EXTRA_WEB_ARGS=("$@")
DATA_MOUNT="${SMARTFIRES_DATA_MOUNT:-/mnt/nvme_drive}"
STOP=0
child_pid=""

stop_supervisor() {
    STOP=1
    if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
        kill -TERM "$child_pid" 2>/dev/null || true
    fi
}
trap stop_supervisor INT TERM

if [[ "$DATA_DIR" == "$DATA_MOUNT" || "$DATA_DIR" == "$DATA_MOUNT"/* ]]; then
    mountpoint -q "$DATA_MOUNT" || {
        echo "Required SmartFires data mount is unavailable: $DATA_MOUNT" >&2
        exit 1
    }
fi
[[ -d "$DATA_DIR" ]] || {
    echo "SmartFires data directory does not exist: $DATA_DIR" >&2
    exit 1
}

restart_delay=2
while (( ! STOP )); do
    smartfires-edge web --port "$PORT" --data-dir "$DATA_DIR" --host 0.0.0.0 --http-port 8080 "${EXTRA_WEB_ARGS[@]}" &
    child_pid=$!
    wait "$child_pid" || true
    child_pid=""
    (( STOP )) && break
    sleep "$restart_delay" &
    child_pid=$!
    wait "$child_pid" || true
    child_pid=""
    restart_delay=$(( restart_delay < 30 ? restart_delay * 2 : 30 ))
done
