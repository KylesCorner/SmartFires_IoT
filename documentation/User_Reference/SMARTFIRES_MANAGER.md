---
name: smartfires-manager
description: Operator reference for the Jetson update, service-management, and gateway flashing script.
category: reference
status: current
last_verified: 2026-09-04
source_refs:
  - edge/smartfires-manager.sh
related_docs:
  - flashing
  - jetson-cheatsheet
  - lora-sniffer
---

# SmartFires build and deployment manager

`edge/smartfires-manager.sh` updates the Jetson checkout/package, manages `smartfires-edge.service`, and flashes the base and sniffer Feathers. It does not flash sensor nodes.

Run it from the repository root:

```bash
chmod +x edge/smartfires-manager.sh
./edge/smartfires-manager.sh --help
```

The script resolves the repository root from its own location, so it may also be invoked by absolute path from another directory.

## Preconditions

- A Git checkout with remote `origin` and the selected remote branch.
- A clean working tree for commands that call `sync_repo`; the script refuses to switch/update with uncommitted changes.
- `git`, `systemctl`, and `sudo` on the Jetson.
- An existing Python virtual environment at `$HOME/.smartfires_venv`, or `SMARTFIRES_VENV` pointing to another one.
- PlatformIO as `pio` or `$HOME/.platformio/penv/bin/pio` for flash commands.
- `smartfires-edge.service` installed for any command that restarts it.
- Stable `/dev/smartfires-base` and `/dev/smartfires-sniffer` symlinks for firmware flashing.

The manager can stop a live service, change branches, install Python packages, and flash connected boards. Review the selected branch and physical USB board identity before running a mutating command.

## Syntax

```text
edge/smartfires-manager.sh [--branch BRANCH] COMMAND
```

The branch defaults to `master`. `SMARTFIRES_BRANCH` changes the environment default; `-b` or `--branch` on the command line takes precedence.

Examples:

```bash
./edge/smartfires-manager.sh status
./edge/smartfires-manager.sh --branch master sync
./edge/smartfires-manager.sh -b gps-power update-edge
```

## Commands

| Command | Actions |
|---|---|
| `status` | Show repository branch/commit, Python environment/package, base/sniffer devices, and service state |
| `sync` | Require a clean tree, verify the remote branch, fetch, switch/create its local tracking branch, then `pull --ff-only` |
| `update-edge` | Stop service, sync repository, force-reinstall the edge package into the venv, restart/verify service |
| `flash-base` | Stop service, sync repository, flash `feather_m0_lora_base` to `/dev/smartfires-base`, wait for re-enumeration, restart service |
| `flash-sniffer` | Same flow using `feather_m0_lora_sniffer` and `/dev/smartfires-sniffer` |
| `flash-gateway` | Stop, sync, flash base then sniffer, restart |
| `deploy` | Stop, sync, reinstall edge package, flash base and sniffer, restart, show status |

With no command or with `--help`, the script prints usage and makes no deployment change.

## Common workflows

Inspect only:

```bash
./edge/smartfires-manager.sh status
```

Update Jetson software without firmware flashing:

```bash
./edge/smartfires-manager.sh update-edge
```

Flash only one gateway board:

```bash
./edge/smartfires-manager.sh flash-base
./edge/smartfires-manager.sh flash-sniffer
```

Full gateway deployment:

```bash
./edge/smartfires-manager.sh --branch master deploy
```

## Exact behavior worth knowing

### Git

The script checks the requested branch with `git ls-remote`, fetches `origin`, checks out an existing local branch or creates a tracking branch, and pulls with `--ff-only`. It never commits, stashes, resets, or force-checks-out user changes.

### Edge package

Installation uses the selected venv's Python:

```text
python -m pip install --upgrade --force-reinstall <repo>/edge/edge-receiver
```

It verifies `<venv>/bin/smartfires-edge --help` after installation. The venv must already exist; the manager does not create it.

### Service

Mutating deployment commands stop `smartfires-edge.service` so it releases the USB devices. Restart calls `systemctl daemon-reload`, restarts the service, waits two seconds, and fails if it is not active. An error may leave the service stopped; check it explicitly.

```bash
sudo systemctl status smartfires-edge.service --no-pager --full
sudo systemctl restart smartfires-edge.service
journalctl -u smartfires-edge.service -f
```

### Firmware

The manager invokes PlatformIO with an explicit project directory, environment, upload target, and stable device path. After upload it waits two seconds, then up to 30 seconds for the same udev symlink to reappear.

The base and sniffer must be physically distinguishable by their udev serial rules. The script trusts those symlinks; it does not independently detect which firmware role a connected Feather should have. Attach a suitable antenna before radio firmware is run.

## Environment variables

```bash
export SMARTFIRES_VENV="$HOME/.smartfires_venv"
export SMARTFIRES_BRANCH=master
```

Avoid pointing `SMARTFIRES_VENV` at a shared/system Python environment. Command-line `--branch` is preferable for a one-off deployment because the selected branch is printed before changes.

## Recovery

If a command fails:

1. Read the reported failing step and service warning.
2. Check `git status --short` and the current branch.
3. Check both stable USB symlinks with `ls -l`.
4. Check `smartfires-edge.service` and its journal.
5. Verify the intended venv contains `bin/python`.
6. Resume with the narrowest command needed; do not automatically rerun full `deploy` if only the service restart failed.

The manager does not roll back a package install or a successful first-board flash if a later step fails. Git remains on the branch selected during sync, and the service may remain stopped.
