#!/usr/bin/env bash

set -Eeuo pipefail

# ============================================================
# SmartFires Build / Deployment Manager
# ============================================================

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PLATFORMIO_DIR="$REPO_ROOT/platformio"
EDGE_DIR="$REPO_ROOT/edge/edge-receiver"

VENV="${SMARTFIRES_VENV:-$HOME/.smartfires_venv}"
PYTHON="$VENV/bin/python"
SMARTFIRES_EDGE="$VENV/bin/smartfires-edge"

SERVICE="smartfires-edge.service"

BASE_PORT="/dev/smartfires-base"
SNIFFER_PORT="/dev/smartfires-sniffer"

BASE_ENV="feather_m0_lora_base"
SNIFFER_ENV="feather_m0_lora_sniffer"

GIT_REMOTE="origin"
GIT_BRANCH="master"

PORT_WAIT_TIMEOUT=30


# ============================================================
# Logging / errors
# ============================================================

log()
{
    printf '\n\033[1;34m[SmartFires]\033[0m %s\n' "$*"
}


success()
{
    printf '\033[1;32m[OK]\033[0m %s\n' "$*"
}


warn()
{
    printf '\033[1;33m[WARN]\033[0m %s\n' "$*" >&2
}


die()
{
    printf '\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2
    exit 1
}


on_error()
{
    local exit_code=$?
    local line_number=$1

    printf '\n\033[1;31m[SmartFires ERROR]\033[0m '
    printf 'Command failed at line %s (exit code %s).\n' \
        "$line_number" "$exit_code"

    printf '\nThe SmartFires service may currently be stopped.\n'
    printf 'Check with:\n'
    printf '  sudo systemctl status %s\n\n' "$SERVICE"

    exit "$exit_code"
}


trap 'on_error $LINENO' ERR


# ============================================================
# Dependency checks
# ============================================================

find_pio()
{
    if command -v pio >/dev/null 2>&1; then
        PIO="$(command -v pio)"
        return
    fi

    if [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
        PIO="$HOME/.platformio/penv/bin/pio"
        return
    fi

    PIO=""
}


require_command()
{
    local cmd="$1"

    command -v "$cmd" >/dev/null 2>&1 ||
        die "Required command not found: $cmd"
}


require_pio()
{
    find_pio

    [[ -n "$PIO" ]] ||
        die "PlatformIO CLI was not found."
}


require_venv()
{
    [[ -x "$PYTHON" ]] ||
        die "SmartFires virtual environment not found: $VENV"

    success "Virtual environment: $VENV"
}


require_repo()
{
    [[ -d "$REPO_ROOT/.git" ]] ||
        die "Could not find Git repository at $REPO_ROOT"
}


require_port()
{
    local port="$1"

    [[ -e "$port" ]] ||
        die "Device not found: $port"
}


preflight()
{
    require_command git
    require_command systemctl
    require_command sudo

    require_repo
    require_venv
}


# ============================================================
# Git
# ============================================================

check_git_tree()
{
    local dirty

    dirty="$(git -C "$REPO_ROOT" status --porcelain)"

    if [[ -n "$dirty" ]]; then
        echo
        git -C "$REPO_ROOT" status --short
        echo

        die "Repository contains uncommitted changes. Refusing to update."
    fi
}


sync_repo()
{
    log "Checking Git repository..."

    require_repo
    check_git_tree

    log "Fetching $GIT_REMOTE/$GIT_BRANCH..."

    git -C "$REPO_ROOT" fetch "$GIT_REMOTE" "$GIT_BRANCH"

    local current_branch

    current_branch="$(
        git -C "$REPO_ROOT" branch --show-current
    )"

    if [[ "$current_branch" != "$GIT_BRANCH" ]]; then
        log "Switching from $current_branch to $GIT_BRANCH..."

        git -C "$REPO_ROOT" checkout "$GIT_BRANCH"
    fi

    log "Updating local $GIT_BRANCH..."

    git -C "$REPO_ROOT" pull \
        --ff-only \
        "$GIT_REMOTE" \
        "$GIT_BRANCH"

    success "Repository updated."

    echo
    git -C "$REPO_ROOT" --no-pager log -1 \
        --format='Commit: %h%nDate:   %cd%nTitle:  %s' \
        --date=iso
}


# ============================================================
# Python edge package
# ============================================================

install_edge()
{
    require_venv

    [[ -f "$EDGE_DIR/pyproject.toml" ]] ||
        die "Could not find smartfires-edge package at $EDGE_DIR"

    log "Installing smartfires-edge into:"
    echo "  $VENV"

    "$PYTHON" -m pip install \
        --upgrade \
        --force-reinstall \
        "$EDGE_DIR"

    [[ -x "$SMARTFIRES_EDGE" ]] ||
        die "smartfires-edge executable was not installed."

    log "Verifying smartfires-edge..."

    "$SMARTFIRES_EDGE" --help >/dev/null

    success "smartfires-edge installed successfully."

    echo
    "$PYTHON" -m pip show smartfires-edge |
        grep -E '^(Name|Version|Location):' || true
}


# ============================================================
# systemd
# ============================================================

service_exists()
{
    systemctl cat "$SERVICE" >/dev/null 2>&1
}


stop_service()
{
    if ! service_exists; then
        warn "$SERVICE is not currently installed."
        return
    fi

    if systemctl is-active --quiet "$SERVICE"; then
        log "Stopping $SERVICE..."

        sudo systemctl stop "$SERVICE"

        success "$SERVICE stopped."
    else
        log "$SERVICE is already stopped."
    fi
}


start_service()
{
    if ! service_exists; then
        die "$SERVICE is not installed."
    fi

    log "Reloading systemd..."

    sudo systemctl daemon-reload

    log "Starting $SERVICE..."

    sudo systemctl restart "$SERVICE"

    sleep 2

    if sudo systemctl is-active --quiet "$SERVICE"; then
        success "$SERVICE is running."
    else
        echo
        sudo systemctl status "$SERVICE" \
            --no-pager \
            --full || true

        die "$SERVICE failed to start."
    fi
}


# ============================================================
# Serial device helpers
# ============================================================

wait_for_port()
{
    local port="$1"
    local timeout="${2:-$PORT_WAIT_TIMEOUT}"

    log "Waiting for $port..."

    for ((i = 0; i < timeout; i++)); do

        if [[ -e "$port" ]]; then
            success "$port is available."
            return 0
        fi

        sleep 1
    done

    die "Timed out waiting for $port after ${timeout}s."
}


show_devices()
{
    echo
    echo "Serial devices:"

    if [[ -e "$BASE_PORT" ]]; then
        printf '  %-30s %s\n' "$BASE_PORT" "[OK]"
        ls -l "$BASE_PORT"
    else
        printf '  %-30s %s\n' "$BASE_PORT" "[MISSING]"
    fi

    echo

    if [[ -e "$SNIFFER_PORT" ]]; then
        printf '  %-30s %s\n' "$SNIFFER_PORT" "[OK]"
        ls -l "$SNIFFER_PORT"
    else
        printf '  %-30s %s\n' "$SNIFFER_PORT" "[MISSING]"
    fi
}


# ============================================================
# Firmware flashing
# ============================================================

flash_device()
{
    local env="$1"
    local port="$2"
    local name="$3"

    require_pio
    require_port "$port"

    log "Flashing SmartFires $name..."
    echo
    echo "  Environment: $env"
    echo "  Port:        $port"
    echo "  PlatformIO:  $PIO"
    echo

    "$PIO" run \
        -d "$PLATFORMIO_DIR" \
        -e "$env" \
        --target upload \
        --upload-port "$port"

    success "$name firmware upload completed."

    #
    # Feather M0 disappears briefly while resetting.
    #
    sleep 2

    wait_for_port "$port"

    success "$name is back online."
}


flash_base()
{
    flash_device \
        "$BASE_ENV" \
        "$BASE_PORT" \
        "base station"
}


flash_sniffer()
{
    flash_device \
        "$SNIFFER_ENV" \
        "$SNIFFER_PORT" \
        "LoRa sniffer"
}


# ============================================================
# High-level operations
# ============================================================

update_edge()
{
    preflight

    log "Beginning SmartFires edge update..."

    stop_service

    sync_repo

    #
    # Important:
    # EDGE_DIR may contain newly-pulled code after sync_repo().
    #
    install_edge

    start_service

    success "SmartFires edge update complete."
}


flash_base_command()
{
    preflight

    stop_service

    flash_base

    start_service

    success "Base station update complete."
}


flash_sniffer_command()
{
    preflight

    stop_service

    flash_sniffer

    start_service

    success "Sniffer update complete."
}


flash_gateway()
{
    preflight

    log "Beginning SmartFires gateway firmware update..."

    stop_service

    flash_base
    flash_sniffer

    start_service

    success "Gateway firmware update complete."
}


deploy()
{
    preflight

    log "=========================================="
    log " SmartFires Full Deployment"
    log "=========================================="

    stop_service

    #
    # Get newest source first.
    #
    sync_repo

    #
    # Install newest Jetson Python application.
    #
    install_edge

    #
    # Flash both USB-connected LoRa boards.
    #
    flash_base
    flash_sniffer

    #
    # Bring edge system back online.
    #
    start_service

    success "=========================================="
    success " SmartFires deployment complete"
    success "=========================================="

    status
}


# ============================================================
# Status
# ============================================================

status()
{
    preflight

    echo
    echo "=========================================="
    echo " SmartFires System Status"
    echo "=========================================="

    echo
    echo "Repository:"
    echo "  $REPO_ROOT"

    git -C "$REPO_ROOT" --no-pager log -1 \
        --format='  Branch:  %D%n  Commit:  %h%n  Date:    %cd%n  Title:   %s' \
        --date=iso

    echo
    echo "Python environment:"
    echo "  Venv:    $VENV"
    echo "  Python:  $("$PYTHON" --version 2>&1)"

    if "$PYTHON" -m pip show smartfires-edge >/dev/null 2>&1; then
        local version

        version="$(
            "$PYTHON" -m pip show smartfires-edge |
                awk '/^Version:/ {print $2}'
        )"

        echo "  Edge:    $version"
    else
        echo "  Edge:    NOT INSTALLED"
    fi

    show_devices

    echo
    echo "systemd:"

    if ! service_exists; then
        echo "  $SERVICE: NOT INSTALLED"
    elif systemctl is-active --quiet "$SERVICE"; then
        echo "  $SERVICE: ACTIVE"
    else
        echo "  $SERVICE: INACTIVE"
    fi

    echo
}


# ============================================================
# Help
# ============================================================

usage()
{
    cat <<EOF

SmartFires Build / Deployment Manager

Usage:

    $(basename "$0") <command>


Commands:

    status
        Show Git, Python, serial device, and systemd status.

    sync
        Pull the newest origin/master.
        Refuses to continue if the repo has uncommitted changes.

    update-edge
        Stop smartfires-edge.service.
        Pull origin/master.
        Reinstall smartfires-edge into ~/.smartfires_venv.
        Restart and verify the service.

    flash-base
        Stop the edge service.
        Flash /dev/smartfires-base.
        Restart the edge service.

    flash-sniffer
        Stop the edge service.
        Flash /dev/smartfires-sniffer.
        Restart the edge service.

    flash-gateway
        Stop the edge service.
        Flash both base and sniffer.
        Restart the edge service.

    deploy
        Full SmartFires deployment:

            stop service
            ↓
            pull origin/master
            ↓
            reinstall smartfires-edge
            ↓
            flash base
            ↓
            flash sniffer
            ↓
            restart service
            ↓
            verify


Environment variables:

    SMARTFIRES_VENV

        Override the SmartFires Python virtual environment.

        Default:

            \$HOME/.smartfires_venv

        Example:

            SMARTFIRES_VENV=/opt/smartfires/venv \\
                $(basename "$0") deploy

EOF
}


# ============================================================
# Main
# ============================================================

case "${1:-}" in

    status)
        status
        ;;

    sync)
        preflight
        sync_repo
        ;;

    update-edge)
        update_edge
        ;;

    flash-base)
        flash_base_command
        ;;

    flash-sniffer)
        flash_sniffer_command
        ;;

    flash-gateway)
        flash_gateway
        ;;

    deploy)
        deploy
        ;;

    help|-h|--help|"")
        usage
        ;;

    *)
        usage
        die "Unknown command: $1"
        ;;

esac
