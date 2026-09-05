# SmartFires IoT

SmartFires is a wildfire telemetry system built from Feather M0 RFM95 sensor nodes, a Feather M0 LoRa base station, and a Jetson Orin Nano edge receiver.

Remote nodes sample SHT31, wind, PA1010D GPS, SPS30, and ICM-20948 sensors. They transmit compact binary bundles over a custom 915 MHz TDMA network. The base assigns node IDs, manages acknowledgements and TX power, and bridges packets over native USB CDC. The Jetson decodes packets, records telemetry, maintains live state, and serves the dashboard.

## Start here

- [`AGENTS.md`](AGENTS.md) — canonical repository guidance and current invariants
- [`documentation/README.md`](documentation/README.md) — documentation index and implementation-status ledger
- [`documentation/SOFTWARE_DESIGN.md`](documentation/SOFTWARE_DESIGN.md) — current end-to-end architecture
- [`platformio/README.md`](platformio/README.md) — firmware targets and workflow
- [`edge/edge-receiver/README.md`](edge/edge-receiver/README.md) — Jetson installation and commands

## Repository layout

```text
platformio/            embedded firmware and native Unity tests
edge/                  Jetson receiver, dashboard, and service helper
documentation/         current architecture, user guides, and plan history
util/                  analysis, plotting, sniffer, and deployment utilities
wind_test_bench/       separate wind-sensor bench project
```

## Current status

The node/base binary pipeline, runtime node assignment, TDMA scheduling, app-layer reliability, duty cycling, watchdog recovery, reset coordination, RX power gating, DMP heading, window markers, Jetson-side persistent UID correlation, ingest, and web dashboard are implemented.

Deferred possibilities are parked in [`documentation/README.md`](documentation/README.md); they are not an active backlog. The two important caveats are that base `ACK_SUMMARY` and direct `TIME_SYNC` still have blocking link-ACK paths that can overrun slot 0, and the native PlatformIO suite has known failures.

## Commands

Install the edge receiver from the repository root:

```bash
python3 -m pip install --use-pep517 -e edge/edge-receiver
smartfires-edge web --port /dev/smartfires-base --http-port 8080
```

Firmware command examples are in [`platformio/commands.txt`](platformio/commands.txt). Run them from `platformio/`; uploading and monitoring require the intended Feather to be connected.

No license file is currently present. Add one before redistributing the repository outside the project team.
