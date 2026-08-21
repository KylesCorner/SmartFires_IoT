#!/usr/bin/env bash

set -Eeuo pipefail

# ============================================================
# SmartFires Build / Deployment Manager
# ============================================================

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PLATFORMIO_DIR="$REPO_ROOT/platformio"
EDGE_DIR="$REPO_ROOT/edge/edge-receiver"

# Existing SmartFires Python virtual environment
VENV="${SMARTFIRES_VENV:-$HOME/.smartfires_venv}"
PYTHON="$VENV/bin/python"
SMARTFIRES_EDGE="$VENV/bin/smartfires-edge"

# systemd
SERVICE="smartfires-edge.service"

# Stable udev device names
BASE_PORT="/dev/smartfires-base"
SNIFFER_PORT="/dev/smartfires-sniffer"

# PlatformIO environments
BASE_ENV="feather_m0_lora_base"
SNIFFER_ENV="feather_m0_lora_sniffer"

# Git
GIT_REMOTE="origin"
GIT_BRANCH="${SMARTFIRES_BRANCH:-master}"

PORT_WAIT_TIMEOUT=30

COMMAND=""


# ============================================================
# Logging
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
    printf '\n\033[1;31m[ERROR]\033[0m %s\n' "$*" >&2
    exit 1
}


on_error()
{
    local exit_code=$?
    local line_number="$1"

    printf '\n\033[1;31m[SmartFires ERROR]\033[0m '
    printf 'Command failed at line %s (exit code %s).\n' \
        "$line_number" "$exit_code"

    printf '\nThe SmartFires service may currently be stopped.\n'
    printf 'Check it with:\n'
    printf '  sudo systemctl status %s\n\n' "$SERVICE"

    exit "$exit_code"
}


trap 'on_error $LINENO' ERR


# ============================================================
# Dependency checks
# ============================================================

require_command()
{
    local cmd="$1"

    command -v "$cmd" >/dev/null 2>&1 ||
        die "Required command not found: $cmd"
}


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
}


require_repo()
{
    [[ -d "$REPO_ROOT/.git" ]] ||
        die "Could not find Git repository at: $REPO_ROOT"
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

        die "Repository contains uncommitted changes. Refusing to switch/update branches."
    fi
}


remote_branch_exists()
{
    git -C "$REPO_ROOT" ls-remote \
        --exit-code \
        --heads \
        "$GIT_REMOTE" \
        "$GIT_BRANCH" \
        >/dev/null 2>&1
}


local_branch_exists()
{
    git -C "$REPO_ROOT" show-ref \
        --verify \
        --quiet \
        "refs/heads/$GIT_BRANCH"
}


sync_repo()
{
    log "Preparing Git branch: $GIT_BRANCH"

    require_repo
    check_git_tree

    log "Checking $GIT_REMOTE/$GIT_BRANCH..."

    if ! remote_branch_exists; then
        die "GitHub branch does not exist: $GIT_REMOTE/$GIT_BRANCH"
    fi

    log "Fetching from GitHub..."

    git -C "$REPO_ROOT" fetch "$GIT_REMOTE"

    local current_branch

    current_branch="$(
        git -C "$REPO_ROOT" branch --show-current
    )"

    if [[ "$current_branch" != "$GIT_BRANCH" ]]; then

        if local_branch_exists; then

            log "Switching to local branch: $GIT_BRANCH"

            git -C "$REPO_ROOT" checkout "$GIT_BRANCH"

        else

            log "Creating local branch from $GIT_REMOTE/$GIT_BRANCH..."

            git -C "$REPO_ROOT" checkout \
                -b "$GIT_BRANCH" \
                --track "$GIT_REMOTE/$GIT_BRANCH"
        fi
    fi

    log "Updating $GIT_BRANCH..."

    git -C "$REPO_ROOT" pull \
        --ff-only \
        "$GIT_REMOTE" \
        "$GIT_BRANCH"

    success "Repository updated successfully."

    echo
    git -C "$REPO_ROOT" --no-pager log -1 \
        --format='Branch: %D%nCommit: %h%nDate:   %cd%nTitle:  %s' \
        --date=iso
}


# ============================================================
# Python edge package
# ============================================================

install_edge()
{
    require_venv

    [[ -f "$EDGE_DIR/pyproject.toml" ]] ||
        die "Could not find smartfires-edge package: $EDGE_DIR"

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
        warn "$SERVICE is not installed."
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

    log "Reloading systemd configuration..."

    sudo systemctl daemon-reload

    log "Starting $SERVICE..."

    sudo systemctl restart "$SERVICE"

    sleep 2

    if systemctl is-active --quiet "$SERVICE"; then

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
# Serial devices
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
        printf '  %-30s [OK]\n' "$BASE_PORT"
        printf '    -> %s\n' "$(readlink -f "$BASE_PORT")"
    else
        printf '  %-30s [MISSING]\n' "$BASE_PORT"
    fi

    if [[ -e "$SNIFFER_PORT" ]]; then
        printf '  %-30s [OK]\n' "$SNIFFER_PORT"
        printf '    -> %s\n' "$(readlink -f "$SNIFFER_PORT")"
    else
        printf '  %-30s [MISSING]\n' "$SNIFFER_PORT"
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
    echo "  Branch:      $GIT_BRANCH"
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
    # Feather M0 resets/re-enumerates after programming.
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
# High-level commands
# ============================================================

sync_command()
{
    preflight
    sync_repo
}


update_edge()
{
    preflight

    log "Beginning SmartFires edge update."

    stop_service

    sync_repo
    install_edge

    start_service

    success "SmartFires edge update complete."
}


flash_base_command()
{
    preflight

    log "Beginning SmartFires base station update."

    stop_service

    sync_repo
    flash_base

    start_service

    success "Base station update complete."
}


flash_sniffer_command()
{
    preflight

    log "Beginning SmartFires sniffer update."

    stop_service

    sync_repo
    flash_sniffer

    start_service

    success "Sniffer update complete."
}


flash_gateway()
{
    preflight

    log "Beginning SmartFires gateway firmware update."

    stop_service

    sync_repo

    flash_base
    flash_sniffer

    start_service

    success "Gateway firmware update complete."
}


deploy()
{
    preflight

    echo
    echo "=========================================="
    echo " SmartFires Full Deployment"
    echo "=========================================="
    echo
    echo " Branch: $GIT_BRANCH"

    stop_service

    #
    # Pull requested branch.
    #
    sync_repo

    #
    # Install Jetson-side Python package.
    #
    install_edge

    #
    # Program both LoRa boards.
    #
    flash_base
    flash_sniffer

    #
    # Bring SmartFires back online.
    #
    start_service

    echo
    success "SmartFires deployment complete."

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
    echo "  Path: $REPO_ROOT"

    local current_branch

    current_branch="$(
        git -C "$REPO_ROOT" branch --show-current
    )"

    echo "  Current branch:  $current_branch"
    echo "  Selected branch: $GIT_BRANCH"

    git -C "$REPO_ROOT" --no-pager log -1 \
        --format='  Commit: %h%n  Date:   %cd%n  Title:  %s' \
        --date=iso

    echo
    echo "Python environment:"
    echo "  Venv:   $VENV"
    echo "  Python: $("$PYTHON" --version 2>&1)"

    if "$PYTHON" -m pip show smartfires-edge >/dev/null 2>&1; then

        local version

        version="$(
            "$PYTHON" -m pip show smartfires-edge |
                awk '/^Version:/ {print $2}'
        )"

        echo "  smartfires-edge: $version"

    else

        echo "  smartfires-edge: NOT INSTALLED"
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

    $(basename "$0") [options] <command>


Options:

    -b, --branch <branch>

        GitHub branch to use.

        Default:
            master

        Examples:

            $(basename "$0") deploy

            $(basename "$0") --branch master deploy

            $(basename "$0") --branch gps-power deploy

            $(basename "$0") -b development flash-base


Commands:

    status

        Show:
          - Git status
          - current branch
          - selected branch
          - Python environment
          - smartfires-edge version
          - base/sniffer devices
          - systemd status


    sync

        Pull the selected GitHub branch.


    update-edge

        Stop smartfires-edge.service
        Pull selected GitHub branch
        Reinstall smartfires-edge
        Restart service


    flash-base

        Stop service
        Pull selected GitHub branch
        Flash /dev/smartfires-base
        Restart service


    flash-sniffer

        Stop service
        Pull selected GitHub branch
        Flash /dev/smartfires-sniffer
        Restart service


    flash-gateway

        Stop service
        Pull selected GitHub branch
        Flash base
        Flash sniffer
        Restart service


    deploy

        Full SmartFires deployment:

            stop service
                |
                v
            pull selected branch
                |
                v
            reinstall smartfires-edge
                |
                v
            flash base
                |
                v
            flash sniffer
                |
                v
            restart service
                |
                v
            verify


Environment variables:

    SMARTFIRES_VENV

        Override the Python virtual environment.

        Default:
            \$HOME/.smartfires_venv


    SMARTFIRES_BRANCH

        Override the default Git branch.

        Default:
            master

EOF
}


# ============================================================
# Argument parsing
# ============================================================

while [[ $# -gt 0 ]]; do

    case "$1" in

        -b|--branch)

            [[ $# -ge 2 ]] ||
                die "$1 requires a branch name."

            GIT_BRANCH="$2"

            shift 2
            ;;


        --branch=*)

            GIT_BRANCH="${1#*=}"

            [[ -n "$GIT_BRANCH" ]] ||
                die "--branch requires a branch name."

            shift
            ;;


        -h|--help)

            usage
            exit 0
            ;;


        status|sync|update-edge|flash-base|flash-sniffer|flash-gateway|deploy)

            if [[ -n "$COMMAND" ]]; then
                die "Only one command may be specified."
            fi

            COMMAND="$1"

            shift
            ;;


        *)

            usage
            die "Unknown argument: $1"
            ;;
    esac

done


# ============================================================
# Main
# ============================================================

if [[ -z "$COMMAND" ]]; then
    usage
    exit 0
fi


case "$COMMAND" in

    status)
        status
        ;;

    sync)
        sync_command
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

esac
