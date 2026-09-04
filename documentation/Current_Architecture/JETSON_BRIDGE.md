---
name: jetson-bridge
description: Frame format and routing between the base station Feather and the Jetson over USB CDC.
category: architecture
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/include/telemetry/BinaryPacket.h
  - platformio/src/app/SmartFiresBaseApp.cpp
  - platformio/include/logging/FramedDebugLogSink.h
  - edge/edge-receiver/src/smartfires_edge/uart_receiver.py
  - edge/edge-receiver/src/smartfires_edge/ingest_service.py
related_docs:
  - packet-reliability
  - tdma-protocol
---

# Jetson bridge

The base Feather and Jetson communicate over native USB CDC (`Serial`) at 115200 baud. `/dev/smartfires-base` is the intended udev-managed device name. The link is bidirectional and shares the Feather's programming USB connector; base `Serial1` is not used.

## Framing

Base to Jetson:

```text
[0xAA][0x55][data_len:u8][rssi:i8][complete LoRa payload][crc8]
```

Jetson to base:

```text
[0xAA][0x55][data_len:u8][complete TIME_SYNC/command payload][crc8]
```

The outer CRC-8/MAXIM covers `data_len` followed by every data byte. In the receive direction, `data_len` includes RSSI. The carried LoRa-format packet also has its own final CRC, except `PKT_DEBUG_LOG`, whose text relies on the outer USB CRC.

`FrameReceiver` scans for `AA 55`, validates the declared length, buffers exactly that many data bytes, checks the outer CRC, extracts RSSI, then dispatches by the inner packet type. A bad length or CRC resets the state machine to magic-byte search without yielding a packet.

## Base-to-Jetson traffic

The base forwards received `AWAKEN`, `BUNDLE`, `FULL_STATE`, `STATUS`, window markers, and `CMD_ACK` frames with the LoRa RSSI attached. Structured firmware log lines are wrapped as `PKT_DEBUG_LOG` frames so plain debug text cannot corrupt the binary stream.

Before forwarding, the base performs local control duties:

- acknowledges `AWAKEN` at the RadioHead link layer;
- assigns or restores the UID hash's node ID;
- tracks telemetry sequence numbers and generates `ACK_SUMMARY`;
- records packet SNR for dynamic TX-power decisions;
- decodes STATUS retry/failure totals and the node's actual TX power/mode;
- consumes command acknowledgements for controller state, then also forwards them.

The Jetson is therefore not in the timing-critical ACK or join loop.

## Jetson-to-base traffic

| Packet | Behavior at the base |
|---|---|
| `TIME_SYNC` | Cache session authority; do not immediately relay it. Periodic base broadcasts use the latest cached value. |
| `CMD_RESET`, node 0 | Reset base state locally: hard or soft according to `reset_type`. |
| `CMD_RESET`, node 2+ | Queue a fire-and-forget LoRa command for the target. |
| `CMD_CALIBRATE` | Queue a fire-and-forget LoRa command; node logs and ACKs. |
| `CMD_SET_TX_POWER` | Queue an absolute dBm and DYNAMIC/STATIC mode for the target. |

Base commands are sent in slot 0 without a RadioHead link ACK. Nodes return `CMD_ACK` in their own slots. A hard-reset ACK is the deliberate exception: the node sends it immediately and without link ACK before rebooting, because reset would destroy a queued response.

The base's locally generated `ACK_SUMMARY` is never supplied by the Jetson.

## Session behavior

On each serial connection, ingest:

1. sends a soft `CMD_RESET` to node 0 (the base);
2. waits briefly for radio reinitialization;
3. sends a new session ID and session-relative time;
4. starts a background TIME_SYNC sender at the configured interval (600 seconds by default).

The base broadcasts its cached time every 50 seconds, using a local fallback if it has no Jetson authority. This shorter radio cadence is intentionally separate from the USB injection cadence.

## Edge outputs

`receive` and `web` use the same reconnecting ingest loop. It expands BUNDLE frames into individual timestamped samples, writes durable telemetry and status data, updates packet-loss/window trackers, persists session metadata, and optionally merges readings from a Jetson-connected ES-W302 anemometer.

STATUS supplies GPS and battery validity, DMP heading/accuracy, lifetime retransmit/fail totals, and applied TX power. The edge may apply magnetic-declination correction when GPS is valid; no calibration payload exchange is implemented.

## Web control surface

- `/api/new_session` signals ingest to create a new session.
- `/api/node_reset` sends a hard reset to a selected node.
- `/api/tx_power` resolves set/increase/decrease and DYNAMIC/STATIC choices into an absolute power command, clamped to 5–13 dBm.
- `/api/command` currently only echoes `{status: queued}` and does not write serial bytes.

The CLI has `receive`, `summary`, `visualize`, and `web`; there is no separate command-sending CLI.

## Limits and failure behavior

- One-byte `data_len` bounds the frame; the largest normal base frame carries RSSI plus a 195-byte bundle.
- The parser counts invalid lengths and outer CRC failures.
- The ingest service reconnects with bounded backoff after serial errors.
- A lost command is detected only indirectly by missing `CMD_ACK` or later STATUS state; LoRa command delivery itself is fire-and-forget.
- USB framing integrity does not replace the inner LoRa CRC or app-layer reliability.
