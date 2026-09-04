---
name: network-test
description: End-to-end LoRa-to-base-to-Jetson integration test procedure using current sensor-node firmware.
category: reference
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/platformio.ini
related_docs:
  - flashing
  - tdma-protocol
  - jetson-bridge
---

# SmartFires network integration test

This procedure verifies a real node through LoRa, the base's USB bridge, and Jetson ingest/dashboard. The repository no longer has a synthetic dummy-node target, so use a node with its sensors attached; values will be real readings.

## Equipment

- One Feather M0 RFM95 base and one or more Feather M0 RFM95 sensor nodes.
- Correct 915 MHz antennas attached before radio operation.
- The node's configured sensors and power supply.
- A Jetson with the edge package installed and stable `/dev/smartfires-base` udev link.
- Optional second Feather flashed as passive sniffer at `/dev/smartfires-sniffer`.

## Prepare firmware

From `SmartFires_IoT/platformio`, flash the base and a debug/Timed node:

```bash
pio run -e feather_m0_lora_base --target upload
pio run -e feather_m0_lora_node_debug --target upload
```

Use `feather_m0_lora_node` instead when validating the production SensorTriggered profile. Confirm every network Feather was built with the same `NUM_SLOTS` (currently 5).

If using a sniffer:

```bash
pio run -e feather_m0_lora_sniffer --target upload
```

## Start the edge receiver

On the Jetson:

```bash
smartfires-edge web \
  --port /dev/smartfires-base \
  --data-dir /mnt/nvme_drive/data \
  --sniffer-port /dev/smartfires-sniffer \
  --num-slots 5
```

Omit sniffer arguments if no sniffer is connected. Open `http://<jetson-ip>:8080`.

Do not open a base serial monitor concurrently; the edge receiver must own `/dev/smartfires-base`. Node monitoring is independent:

```bash
SFDBG_SRC=boot,tdma,radio,packet,duty SFDBG_MIN_LEVEL=I \
  pio device monitor -e feather_m0_lora_node_debug
```

## Expected join sequence

1. The node emits a 12-byte `AWAKEN` and retries every five seconds until it receives assignment.
2. The base acknowledges `AWAKEN`, assigns a node ID beginning at 2, and sends direct `TIME_SYNC`.
3. The node adopts the ID and starts its duty cycle.
4. The base forwards `AWAKEN` to the Jetson; the dashboard records the UID/reset diagnostics.
5. The node produces telemetry in its assigned slot. The base forwards it and later sends `ACK_SUMMARY` in slot 0.

Useful node log messages include `time_sync_received`, telemetry enqueue/TX events, and `ack_summary_received`. Useful base logs include `awaken_rx`, assignment/sync, `rx_lora`, and `tx_ack_summary_local`.

## Timed-profile expectations

`node_debug` has a nominal 75-second cycle:

- 10 seconds sensor warmup;
- 30 samples at 1-second intervals;
- two complete 15-sample bundles;
- `WINDOW_END`, final TX drain, then roughly 35 seconds standby;
- `WINDOW_BEGIN` on the next wake.

STATUS is compiled at 15-second cadence, though sleep and queue scheduling affect when it appears on air. A four-node-capacity frame is 4.5 seconds, and the app-layer retry gate is currently 9 seconds.

## Validate

Check all of the following:

- Dashboard/base link reports connected.
- Node ID is 2 or greater and the same UID keeps its assignment across a new session.
- BUNDLE rows contain plausible sensor values and increasing session timestamps.
- STATUS shows GPS/battery validity, heading if valid, retry/fail totals, and TX power.
- Packet-loss counters stabilize rather than growing continuously.
- Timed window begin/end state changes match node wake/sleep behavior.
- Sniffer slot assignment and guard-jitter views match `NUM_SLOTS=5` if enabled.
- Session data appears beneath the chosen data directory.

## Control tests

From the dashboard:

1. Pin a node to a safe STATIC power and confirm a later STATUS reports the applied 5–13 dBm value and static mode.
2. Return it to DYNAMIC and confirm STATUS updates.
3. Use the node reset control. A hard reset should yield a `CMD_ACK` if received, a new `AWAKEN`, reset-cause diagnostics, reassignment, and resumed telemetry.
4. Start a new session and confirm the base soft-reset/time-sync handshake completes.

Calibration is not an end-to-end operator test: the generic `/api/command` route does not transmit, and node calibration behavior is intentionally log-and-ACK.

## Failure isolation

| Symptom | Check |
|---|---|
| No `/dev/smartfires-base` | udev serial match, USB cable, service ownership |
| Node repeats AWAKEN forever | base powered, antenna/frequency, assignment capacity, matching `NUM_SLOTS` |
| Assignment but no telemetry | sensor initialization, duty phase, queue logs, sync freshness |
| Base sees packets but dashboard does not | USB ownership, framing CRC/length failures, edge process logs |
| Retransmissions grow | base ACK logs, node slot-0 RX, 150 ms wake-ahead, RF conditions |
| Sniffer timing looks wrong | match `--num-slots` and modem settings; remember RSSI is local to sniffer |
| Reset command appears lost | command is LoRa fire-and-forget; inspect `CMD_ACK`, reboot AWAKEN, and STATUS |

Known limitations during this test are the blocking base `ACK_SUMMARY`/direct-sync slot-overrun risk and the separate native-test repair backlog. Record whether a failure is reproducible on hardware rather than assuming those known items explain it.
