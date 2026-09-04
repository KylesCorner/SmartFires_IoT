# SmartFires IoT agent guide

This file is the canonical repository guidance for Codex and other coding agents. It applies to the entire `SmartFires_IoT/` tree.

## Safety boundary

- Do not run `pio run`, `pio test`, `pio device monitor`, an upload target, or another hardware-facing PlatformIO command unless the user explicitly asks for that execution in the current session.
- For embedded validation, inspect the code and give the exact PlatformIO command the user should run. Hardware availability, board selection, serial-monitor ownership, and the local PlatformIO installation are session-specific.
- Read-only source inspection and non-hardware Python validation are safe defaults.

## System in one page

SmartFires is a wildfire telemetry network:

```text
Feather M0 RFM95 sensor node(s)
  SHT31 + wind + PA1010D GPS + SPS30 + ICM-20948
                |
                | LoRa 915 MHz, custom binary protocol, TDMA
                v
Feather M0 RFM95 base station
                |
                | native USB CDC (`Serial`), 115200 baud
                v
Jetson Orin Nano running `smartfires-edge`
```

Each remote unit uses one Feather; there is no ESP32-to-Feather UART split. `Serial1` on a node is used by the SPS30. The base-to-Jetson link uses native USB CDC, not `Serial1`.

The current deployed network geometry is defined once in `platformio/platformio.ini`: `NUM_SLOTS=5`. Slot 0 belongs to the base, so this supports four assigned node IDs. Nodes boot from an identity derived from the SAMD21 UID, broadcast `AWAKEN`, and receive a runtime node ID from the base. Real assigned IDs therefore start at 2; address 1 is the base.

## Authority and documentation

Use sources in this order when statements disagree:

1. Shipped code and `platformio/platformio.ini`.
2. `documentation/Current_Architecture/`, `documentation/SOFTWARE_DESIGN.md`, and user references marked `status: current`.
3. `documentation/Pending_Plans/` for proposed work only.
4. `documentation/Completed_Plans/` and `documentation/Project_Progress/` as historical records, not current behavior.

Start at `documentation/README.md`. Current architecture docs use YAML frontmatter defined by `documentation/DOC_FRONTMATTER.md`; firmware files use the reciprocal header format in `documentation/CODE_FRONTMATTER.md`.

When changing behavior:

- Update every current doc whose `source_refs` names the changed file and advance `last_verified` only after checking it line-by-line.
- Keep firmware `docs:` backlinks exactly symmetric with documentation `source_refs`.
- Run `python3 documentation/check_doc_freshness.py` and `python3 documentation/check_code_headers.py`.
- Keep protocol changes synchronized between `platformio/include/telemetry/BinaryPacket.h` and `edge/edge-receiver/src/smartfires_edge/packet.py`.
- Keep sensor implementations writing `SensorSnapshot`; wire encoding belongs in `PacketHandler`/`BinaryPacket`.

## Current runtime facts

- LoRa: 915 MHz, base address/node ID 1, nominal TX power 13 dBm.
- TDMA: 900 ms slots, 20 ms guard at each edge, 150 ms node RX wake-ahead, and a 22-minute stale-sync timeout.
- Reliability: node builds use app-layer `ACK_SUMMARY`, an eight-packet pending window, at most three attempts, and 30-second maximum pending age. Steady telemetry is fire-and-forget at the RadioHead link layer; selected control paths still use link ACKs.
- Packet sizes include the embedded CRC: `AWAKEN` 12 bytes (legacy 9 accepted), `TIME_SYNC` 14, `ACK_SUMMARY` 10, `WINDOW_BEGIN/END` 17, `STATUS` 27, `CMD_CALIBRATE/RESET` 8, `CMD_SET_TX_POWER` 9, `CMD_ACK` 12, and `BUNDLE` up to 195.
- Active node builds emit `STATUS` every 15 seconds because all current node environments override the 15-minute header fallback.
- The production node uses SensorTriggered duty cycling. `node_debug` and `node_timed` use Timed duty cycling; `node_hybrid` remains available. A Timed 30-second active window at a 1-second sample interval produces 30 samples, or two complete 15-sample bundles.
- `CMD_RESET` is implemented end-to-end, including `node_id=0` base reset and the dashboard `/api/node_reset` endpoint. `CMD_CALIBRATE` is intentionally log-and-ACK because the DMP self-calibrates. The generic `/api/command` route is still an echo stub.
- Per-node TX power control is shipped. The base uses received SNR and STATUS retry/failure counters, while `/api/tx_power` supplies dynamic/static operator control. Thresholds are still untuned; the node clamps commands to 5–13 dBm.
- The Jetson CLI subcommands are `receive`, `summary`, `visualize`, and `web`; the web default is port 8080. The stable base device is `/dev/smartfires-base`.
- Known open work is indexed in `documentation/README.md`. In particular, blocking `sendToWait()` remains on base `ACK_SUMMARY` and direct `TIME_SYNC` paths, and the native PlatformIO suite has known failures described in `Pending_Plans/NATIVE_TEST_REPAIR.md`.

## Repository map

```text
platformio/                     Feather firmware and native Unity tests
  include/config/              authoritative firmware tunables
  include/telemetry/           C++ wire format
  src/main.cpp                 role composition and power-test entrypoints
  src/app/                     node/base coordinators
  src/radio/                   TDMA, reliability, queueing
edge/edge-receiver/            installable Jetson Python package
  src/smartfires_edge/config.py authoritative edge defaults
  src/smartfires_edge/packet.py Python wire-format mirror
  src/smartfires_edge/main.py   CLI entrypoint
documentation/                 current docs, references, and plan history
util/                          standalone analysis and deployment utilities
wind_test_bench/               separate wind-sensor bench firmware
```

## PlatformIO environments

- `native`: host Unity tests.
- `feather_m0_lora_base`: Feather connected to the Jetson.
- `feather_m0_lora_node`: production SensorTriggered node.
- `feather_m0_lora_node_debug`: default environment, Timed node with structured monitor filters.
- `feather_m0_lora_node_timed`, `feather_m0_lora_node_hybrid`: alternate duty-cycle profiles.
- `feather_m0_lora_sniffer`: passive NDJSON sniffer.
- `feather_m0_power_mcu_run`, `..._mcu_standby`, `..._i2c_idle`, `..._radio_standby`, `..._radio_rx`, `..._sht31`, `..._imu`, `..._gps`, `..._sps30`, `..._wind`: isolated power measurements.

There is no current dummy-node or sensor-probe environment.

Changing `NUM_SLOTS` requires rebuilding and reflashing the base and every node, then matching `DEFAULT_NUM_SLOTS` in the edge config. A mismatch changes the frame period and can prevent additional nodes from ever receiving an assignment.

## Useful non-hardware checks

Run from the repository root:

```bash
python3 documentation/check_doc_freshness.py
python3 documentation/check_code_headers.py
python3 -m compileall -q edge/edge-receiver/src
```

If the user requests embedded execution, run PlatformIO commands from `platformio/`; otherwise report the exact command without executing it.
