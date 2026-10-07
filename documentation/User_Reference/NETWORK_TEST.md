---
name: network-test
description: End-to-end LoRa-to-base-to-Jetson integration test procedure using current sensor-node firmware.
category: reference
status: current
last_verified: 2026-10-07
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

Use `feather_m0_lora_node` instead when validating the production Continuous mode. Confirm the base, every node, and the optional sniffer were built with the same `SMARTFIRES_NETWORK_PROFILE`; matching `NUM_SLOTS=5` alone is not sufficient.

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

1. The node emits a 12-byte fire-and-forget `AWAKEN` and retries at the selected 5/7/12/27-second cadence plus bounded jitter until it receives assignment.
2. The base assigns a node ID beginning at 2 and sends fire-and-forget direct `TIME_SYNC`.
3. The node adopts the ID and starts its duty cycle.
4. The base forwards `AWAKEN` to the Jetson; the dashboard records the UID/reset diagnostics.
5. The node produces telemetry in its assigned slot. The base forwards it and later sends `ACK_SUMMARY` in slot 0.

Useful node log messages include `time_sync_received`, telemetry enqueue/TX events, and `ack_summary_received`. Useful base logs include `awaken_rx`, assignment/sync, `rx_lora`, and `tx_ack_summary_local`.

## Timed-mode expectations

At SF7, `node_debug` has a nominal 75-second cycle:

- 10 seconds sensor warmup;
- 30 samples at 1-second intervals;
- two complete 15-sample bundles;
- `WINDOW_END`, final TX drain, then roughly 35 seconds standby;
- `WINDOW_BEGIN` on the next wake.

SF9/SF10 retain the 30-sample window with 80/90-second cycles. SF12 uses 16 samples as two eight-sample bundles over a 64-second active window and a 150-second cycle. STATUS cadence is 15/30/120/300 seconds, frame length is 4.5/6.5/11/25 seconds, and the app-layer retry gate is 9/13/22/50 seconds for SF7/SF9/SF10/SF12.

## Validate

Check all of the following:

- Dashboard/base link reports connected.
- Before the first announcement, the header shows `SF unknown`; within the base's five-second announcement interval it shows the selected identity, including `SF12 · 250 kHz · 4/5 · base` for the SF12 trial.
- `/api/network_profile` reports the selected ID/fingerprint, `source: base`, and no mismatches. If a sniffer is present, its fingerprint agrees.
- Node ID is 2 or greater and the same UID keeps its assignment across a new session.
- BUNDLE rows contain plausible sensor values and increasing session timestamps.
- STATUS shows GPS/battery validity, heading if valid, retry/fail totals, and TX power.
- Packet-loss counters stabilize rather than growing continuously.
- Timed window begin/end state changes match node wake/sleep behavior.
- Sniffer slot assignment and guard-jitter views match `NUM_SLOTS=5` if enabled.
- Sniffer slot width and guards match the selected profile if enabled.
- `session.json` records the full `network_profile` object and source beside the session data.

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
| Node repeats AWAKEN forever | base powered, antenna/frequency, assignment capacity, matching network profile |
| Assignment but no telemetry | sensor initialization, duty phase, queue logs, sync freshness |
| Base sees packets but dashboard does not | USB ownership, framing CRC/length failures, edge process logs |
| Retransmissions grow | base ACK logs, node slot-0 RX, selected wake-ahead, RF conditions |
| Sniffer timing looks wrong | confirm base/sniffer fingerprints match; remember RSSI is local to sniffer |
| Reset command appears lost | command is LoRa fire-and-forget; inspect `CMD_ACK`, reboot AWAKEN, and STATUS |

For the SF12 trial, begin at STATIC 13 dBm and record maximum-BUNDLE timing, slot/guard violations, join time, delivery ratio, latency, retries/failures, watchdog resets, and queue behavior before drawing range conclusions. The SF9/SF10/SF12 timing margins are provisional until measured, and a successful one-node test does not replace the planned four-node soak. Record whether every failure is reproducible on hardware.
