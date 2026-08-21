# SmartFires Build Manager

The SmartFires build manager provides a single command-line interface for updating the Jetson software and flashing the SmartFires LoRa gateway boards.

It is intended to reduce friction when deploying updates to SmartFires systems by handling:

* Git branch selection and updates
* `smartfires-edge` Python package installation
* `smartfires-edge.service` management
* Base station firmware flashing
* LoRa sniffer firmware flashing
* Full gateway deployments
* Basic deployment status checks

## Location

The manager script is located at:

```text
deploy/smartfires-manager.sh
```

Run commands from anywhere using the full path, or from the repository root:

```bash
./deploy/smartfires-manager.sh
```

Make sure the script is executable:

```bash
chmod +x deploy/smartfires-manager.sh
```

---

# Requirements

The Jetson must have:

* Git
* Python 3
* Existing SmartFires virtual environment
* PlatformIO
* systemd
* GitHub authentication
* SmartFires udev rules installed

The default Python virtual environment is:

```text
~/.smartfires_venv
```

The manager expects the following stable serial device names:

```text
/dev/smartfires-base
/dev/smartfires-sniffer
```

These should be provided by the SmartFires udev rules.

The expected PlatformIO firmware environments are:

```text
feather_m0_lora_base
feather_m0_lora_sniffer
```

The expected systemd service is:

```text
smartfires-edge.service
```

---

# GitHub Authentication

The Jetson must be able to access the SmartFires GitHub repository.

For a fine-grained GitHub Personal Access Token used for both pulling and pushing, configure the token with:

```text
Repository access:
    SmartFires_IoT

Repository permissions:
    Contents: Read and write
```

Authenticate GitHub CLI:

```bash
gh auth login --with-token
```

Paste the token, then configure Git to use the GitHub CLI credentials:

```bash
gh auth setup-git
```

Verify authentication:

```bash
gh auth status
```

Test Git access:

```bash
git fetch origin
```

---

# Basic Usage

Show help:

```bash
./deploy/smartfires-manager.sh --help
```

or:

```bash
./deploy/smartfires-manager.sh
```

The default Git branch is:

```text
master
```

---

# Branch Selection

Use `-b` or `--branch` to deploy from another GitHub branch.

Production deployment from `master`:

```bash
./deploy/smartfires-manager.sh deploy
```

Explicitly select `master`:

```bash
./deploy/smartfires-manager.sh --branch master deploy
```

Deploy a feature branch:

```bash
./deploy/smartfires-manager.sh --branch feature/mcu-duty-cycle deploy
```

Short form:

```bash
./deploy/smartfires-manager.sh -b feature/mcu-duty-cycle deploy
```

The branch option may be placed before or after the command:

```bash
./deploy/smartfires-manager.sh -b feature/test deploy
```

or:

```bash
./deploy/smartfires-manager.sh deploy -b feature/test
```

Before switching or updating branches, the manager checks for uncommitted Git changes.

If the working tree is dirty, the operation is aborted.

---

# Commands

## `status`

Display the current SmartFires deployment state.

```bash
./deploy/smartfires-manager.sh status
```

Shows:

* Repository path
* Current Git branch
* Selected deployment branch
* Current commit
* Python virtual environment
* Installed `smartfires-edge` version
* Base station device status
* Sniffer device status
* systemd service status

Example:

```text
==========================================
 SmartFires System Status
==========================================

Repository:
  Path: /home/smartfires/repos/smartfires
  Current branch:  master
  Selected branch: master
  Commit: abc1234

Python environment:
  Venv: /home/smartfires/.smartfires_venv
  Python: Python 3.x
  smartfires-edge: 0.1.0

Serial devices:
  /dev/smartfires-base            [OK]
  /dev/smartfires-sniffer         [OK]

systemd:
  smartfires-edge.service: ACTIVE
```

---

## `sync`

Update the local repository from the selected GitHub branch.

```bash
./deploy/smartfires-manager.sh sync
```

For another branch:

```bash
./deploy/smartfires-manager.sh -b feature/mcu-duty-cycle sync
```

The manager performs a fast-forward-only update.

Conceptually:

```bash
git fetch origin
git checkout <branch>
git pull --ff-only origin <branch>
```

A merge commit will not be created automatically.

---

## `update-edge`

Update only the Jetson-side Python application.

```bash
./deploy/smartfires-manager.sh update-edge
```

Sequence:

```text
stop smartfires-edge.service
        |
        v
pull selected Git branch
        |
        v
reinstall smartfires-edge
        |
        v
restart smartfires-edge.service
        |
        v
verify service
```

The package is installed into:

```text
~/.smartfires_venv
```

from:

```text
edge/edge-receiver
```

Use this command when the Python edge application changed but the Feather firmware did not.

---

## `flash-base`

Update the LoRa base station firmware.

```bash
./deploy/smartfires-manager.sh flash-base
```

The manager:

1. Stops `smartfires-edge.service`
2. Pulls the selected Git branch
3. Builds `feather_m0_lora_base`
4. Flashes `/dev/smartfires-base`
5. Waits for the USB device to return
6. Restarts `smartfires-edge.service`

Feature branch example:

```bash
./deploy/smartfires-manager.sh \
    -b feature/base-change \
    flash-base
```

---

## `flash-sniffer`

Update the LoRa sniffer firmware.

```bash
./deploy/smartfires-manager.sh flash-sniffer
```

The manager uses:

```text
PlatformIO environment:
    feather_m0_lora_sniffer

Serial device:
    /dev/smartfires-sniffer
```

Feature branch example:

```bash
./deploy/smartfires-manager.sh \
    -b feature/sniffer-change \
    flash-sniffer
```

---

## `flash-gateway`

Flash both USB-connected LoRa boards.

```bash
./deploy/smartfires-manager.sh flash-gateway
```

Sequence:

```text
stop edge service
      |
      v
pull selected branch
      |
      v
flash base
      |
      v
flash sniffer
      |
      v
restart edge service
```

This command does not reinstall the Python edge package.

---

## `deploy`

Perform a complete SmartFires gateway deployment.

```bash
./deploy/smartfires-manager.sh deploy
```

This is the primary deployment command.

Sequence:

```text
stop smartfires-edge.service
        |
        v
fetch selected Git branch
        |
        v
checkout/update selected branch
        |
        v
reinstall smartfires-edge
        |
        v
flash LoRa base station
        |
        v
flash LoRa sniffer
        |
        v
restart smartfires-edge.service
        |
        v
verify deployment
```

Production:

```bash
./deploy/smartfires-manager.sh deploy
```

Feature testing:

```bash
./deploy/smartfires-manager.sh \
    -b feature/mcu-duty-cycle \
    deploy
```

---

# Virtual Environment

The manager uses the existing SmartFires virtual environment:

```text
~/.smartfires_venv
```

It does not create or replace the environment.

The package is installed using the Python interpreter inside that environment.

Conceptually:

```bash
~/.smartfires_venv/bin/python \
    -m pip install \
    --upgrade \
    --force-reinstall \
    ./edge/edge-receiver
```

You do not need to manually activate the environment before running the manager.

This works:

```bash
./deploy/smartfires-manager.sh deploy
```

even if the shell prompt does not currently show:

```text
(.smartfires_venv)
```

---

# Overriding the Virtual Environment

Set `SMARTFIRES_VENV` if a system uses a different environment location.

Example:

```bash
SMARTFIRES_VENV=/opt/smartfires/venv \
    ./deploy/smartfires-manager.sh deploy
```

---

# Overriding the Default Branch

The default branch is:

```text
master
```

It can also be overridden with an environment variable:

```bash
SMARTFIRES_BRANCH=feature/test \
    ./deploy/smartfires-manager.sh deploy
```

Command-line `--branch` selection is preferred for interactive use:

```bash
./deploy/smartfires-manager.sh \
    --branch feature/test \
    deploy
```

---

# PlatformIO

The manager searches for PlatformIO in this order:

```text
pio
```

from the system `PATH`, followed by:

```text
~/.platformio/penv/bin/pio
```

The firmware project is expected at:

```text
platformio/
```

The base station is built with:

```text
feather_m0_lora_base
```

The sniffer is built with:

```text
feather_m0_lora_sniffer
```

---

# Why the Edge Service Is Stopped During Flashing

`smartfires-edge` normally holds the base and sniffer serial ports open.

These devices are:

```text
/dev/smartfires-base
/dev/smartfires-sniffer
```

PlatformIO also needs access to those devices when programming the Feather boards.

The manager therefore stops:

```text
smartfires-edge.service
```

before flashing.

After programming completes and the USB devices re-enumerate, the manager restarts the service.

---

# Safety Behavior

The build manager intentionally fails rather than attempting to recover automatically from an uncertain deployment state.

For example, deployment stops if:

* The Git working tree contains uncommitted changes
* The requested Git branch does not exist
* `git pull --ff-only` fails
* The SmartFires virtual environment is missing
* Python package installation fails
* PlatformIO is unavailable
* A required Feather is missing
* Firmware compilation fails
* Firmware flashing fails
* A serial device does not return after programming
* `smartfires-edge.service` fails to start

If a deployment fails after the service has been stopped, the service may intentionally remain stopped.

Check it with:

```bash
sudo systemctl status smartfires-edge.service
```

View recent logs with:

```bash
journalctl -u smartfires-edge.service -n 100 --no-pager
```

Follow logs live:

```bash
journalctl -fu smartfires-edge.service
```

Restart manually if appropriate:

```bash
sudo systemctl restart smartfires-edge.service
```

---

# Common Workflows

## Update everything from production

```bash
./deploy/smartfires-manager.sh deploy
```

## Test a feature branch on the full gateway

```bash
./deploy/smartfires-manager.sh \
    -b feature/mcu-duty-cycle \
    deploy
```

## Update only the Jetson Python package

```bash
./deploy/smartfires-manager.sh update-edge
```

## Update only the base station

```bash
./deploy/smartfires-manager.sh flash-base
```

## Update only the sniffer

```bash
./deploy/smartfires-manager.sh flash-sniffer
```

## Update both Feathers without reinstalling Python

```bash
./deploy/smartfires-manager.sh flash-gateway
```

## Check the system before deployment

```bash
./deploy/smartfires-manager.sh status
```

---

# Recommended Production Workflow

Before deploying:

```bash
git status
```

Then check the gateway:

```bash
./deploy/smartfires-manager.sh status
```

Deploy production:

```bash
./deploy/smartfires-manager.sh deploy
```

Verify:

```bash
./deploy/smartfires-manager.sh status
```

Check the service:

```bash
sudo systemctl status smartfires-edge.service
```

For a development branch:

```bash
./deploy/smartfires-manager.sh \
    -b feature/my-feature \
    deploy
```

After testing, return the gateway to production with:

```bash
./deploy/smartfires-manager.sh \
    -b master \
    deploy
```

---

# Future Work

Planned extensions to the build manager may include:

* Mass flashing SmartFires sensor nodes
* Automatic USB device discovery
* Node serial number tracking
* Firmware version reporting
* Git commit SHA recording for flashed devices
* Deployment logs
* Deployment manifests
* PyPI-based `smartfires-edge` installation
* Package publishing workflow
* Firmware release artifacts
* Automated health checks after deployment

The long-term goal is for the SmartFires build manager to provide one consistent interface for deploying the complete system:

```text
GitHub
   |
   +---- Jetson software
   |
   +---- Base firmware
   |
   +---- Sniffer firmware
   |
   +---- Node firmware
            |
            v
     SmartFires deployment
```

