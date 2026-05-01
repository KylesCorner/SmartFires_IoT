# SmartFires IoT Firmware

PlatformIO firmware for the SmartFires Feather M0 LoRa deployment.

See `PHASE_PROGRESS.md` for the staged reliability rollout status.

## What This Project Builds

This workspace contains three active firmware targets:

- `feather_m0_lora_base`
  Base station / edge relay. It listens on LoRa, forwards received packets to the Jetson over `Serial1`, and sends `TIME_SYNC` / `ACK_SUMMARY` packets back to nodes.

- `feather_m0_lora_node`
  Real sensor node. It reads SHT31, GPS, ICM-20948, and SPS30 sensors, joins the TDMA network, and sends telemetry bundles to the base.

- `feather_m0_lora_node_dummy`
  Synthetic node used for radio-path and packet debugging. It uses dummy sensor data but exercises the same TDMA, ACK, and forwarding path as the real node.

There is also a native unit-test environment:

- `native`
  Runs Unity tests on the host machine without Arduino hardware.

## Current Network Model

The current TDMA layout is a 4-entity network:

- slot 0: base station
- slot 1: node ID 2
- slot 2: node ID 3
- slot 3: node ID 4

Nodes do not boot with a permanent TDMA slot. Instead, they join in two stages:

1. A node boots with an unassigned TDMA/node ID and a temporary radio address derived from its board UID hash.
2. The node sends an out-of-band `AWAKEN` handshake containing that UID hash.
3. The base assigns one of the three node IDs and returns it in a direct `TIME_SYNC` reply.
4. The node switches its local radio address at runtime and then participates in the TDMA schedule.

This avoids the circular dependency of needing a TDMA slot in order to request a TDMA slot.

## Packet / Runtime Notes

- `AWAKEN` is a pre-TDMA join packet and is sent directly instead of through the TDMA transmit queue.
- `TIME_SYNC` is used both for periodic broadcast sync and for direct assignment during join.
- `ACK_SUMMARY` is the base-to-node app-layer reliability summary.
- Telemetry bundling is intentionally buffered: one reference snapshot plus `kBundleMaxDeltas` delta samples are accumulated before a `BUNDLE` packet is emitted.
- In the normal node build, duty cycling is disabled by default and the node should remain in continuous sampling mode once sync has been acquired.
- The current node environments default to `AppLayerAckSummary` telemetry mode. Strict per-packet link ACK mode is still available via `SMARTFIRES_TDMA_RELIABILITY_MODE=0`.
- The base now emits `[BaseApp][SEQ20]` debug summaries so you can see how many packets arrived in each 20-sequence receive window.
- Phase 3 has started with a short retransmit holdoff after each fresh telemetry send so idle retry traffic does not immediately crowd out newly generated data.
- When the app-layer pending window fills, the node now evicts the stalest retransmit candidate first rather than dropping the raw oldest pending packet.

## Source Layout

- `src/app/`
  High-level base and node application state machines.

- `src/platform/`
  Hardware and third-party adapter implementations.

- `src/power/`
  Battery and duty-cycle logic.

- `src/radio/`
  TDMA clocking, queuing, packet handling, and reliability logic.

- `src/sensors/`
  Sensor wrappers that normalize hardware into a common interface.

- `src/telemetry/`
  Bundle/status encoding helpers.

- `include/`
  Public headers for the modules above.

- `test/`
  Native Unity tests and test support shims/fakes.

## Common Commands

See `commands.txt` for the short command list. Typical workflows are:

```bash
pio run -e feather_m0_lora_base
pio run -e feather_m0_lora_node
pio run -e feather_m0_lora_node_dummy

pio run -e feather_m0_lora_base --target upload
pio run -e feather_m0_lora_node --target upload
pio run -e feather_m0_lora_node_dummy --target upload

pio device monitor -e feather_m0_lora_base
pio device monitor -e feather_m0_lora_node

pio test -e native
pio test -e native -f test_duty_cycle_controller
```

If `pio` is not on the shell path, use `platformio` instead.

## Reliability Mode Selection

- `SMARTFIRES_TDMA_RELIABILITY_MODE=0`
  Strict per-packet link ACK telemetry mode.

- `SMARTFIRES_TDMA_RELIABILITY_MODE=1`
  Fire-and-forget telemetry with app-layer `ACK_SUMMARY` reliability.

The current `feather_m0_lora_node` and `feather_m0_lora_node_dummy` environments build with mode `1`.

## Current Debugging Expectations

- On node boot, expect `AWAKEN` retries until `TIME_SYNC` is received.
- After assignment, the node should log its assigned node ID and slot.
- Continuous `Telemetry ready -- snapshot built` logs do not imply that every cycle sends a bundle; bundle emission is gated by the packet accumulator.
- `ACK_SUMMARY base_seq=N mask=0x0` means the base has contiguous acknowledgment through `N` and has not additionally acknowledged any newer out-of-order packets.
