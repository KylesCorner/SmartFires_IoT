---
name: uart-jetson-bridge
description: Frame format and routing between the base station Feather and the Jetson over USB CDC.
category: architecture
status: current
last_verified: 2026-08-17
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

# Jetson Bridge

The Feather M0 base station and the Jetson Orin Nano communicate over a
bidirectional link carried on native USB CDC (`Serial` on the Feather — the
same port used for flashing). A lightweight framing protocol wraps LoRa
payloads for reliable transport over the serial link.

This link was originally a hardware UART (`Serial1` ↔ `/dev/ttyTHS1`); it was
migrated to USB so the base station matches the sniffer Feather's transport
(`main_lora_sniffer.cpp`) and to free up `Serial1`'s pins. The on-wire frame
format below is unchanged by that migration — it's transport-agnostic.

## Physical Setup

| Side | Port | Baud | Notes |
|---|---|---|---|
| Feather M0 base | `Serial` (native USB) | 115 200 | Same USB cable used to flash the board |
| Jetson Orin Nano | udev symlink, e.g. `/dev/smartfires-base` | 115 200 | See "Disambiguating base vs. sniffer" below |

The Feather's old `Serial1` UART pins are no longer wired to the Jetson and
carry no debug traffic on the base build — `Serial1` is unused there (it's
only used on the node build, for the SPS30 driver). Debug logs are already
multiplexed onto the same USB link as `PKT_DEBUG_LOG` frames: `main.cpp`
constructs a `FramedDebugLogSink` wrapping `Serial`, and all `LOG_INFO` /
`LOG_WARN` / etc. calls route through it, so debug text and the binary
Jetson protocol share one USB stream, distinguished by `PKT_DEBUG_LOG`
framing.

### Disambiguating base vs. sniffer

Both the base and the sniffer Feathers enumerate as generic USB CDC-ACM
devices with identical VID/PID, so `/dev/ttyACM0`/`ttyACM1` can swap on
reboot or reconnect. Add a udev rule keyed on each board's USB serial number
to get stable symlinks:

```bash
udevadm info -a -n /dev/ttyACM0 | grep '{serial}'   # run once per board, alone
```

then create `/etc/udev/rules.d/99-smartfires.rules` with one `SYMLINK+=`
rule per board (matching on `ATTRS{serial}`), producing
`/dev/smartfires-base` and `/dev/smartfires-sniffer`. See
`JETSON_CHEATSHEET.md` for the exact invocation using these symlinks.

## Frame Format

All frames in both directions use the same envelope:

```
[0xAA][0x55][len: u8][... data bytes (len bytes) ...][crc8: u8]
```

- `0xAA 0x55` — magic bytes (FRAME_M0, FRAME_M1)
- `len` — number of data bytes that follow (covers `rssi` + LoRa payload for
  Feather→Jetson frames; covers the LoRa payload for Jetson→Feather frames)
- `crc8` — CRC-8/MAXIM (polynomial 0x31) over the `len` byte plus all data bytes

### Feather → Jetson: base UART frame

Wraps a received LoRa payload with RSSI metadata.

```
[0xAA][0x55][len][rssi: i8][LoRa payload bytes][crc8]
```

| Packet type | LoRa payload | `len` | Total frame |
|---|---|---:|---:|
| `AWAKEN` | 12 bytes | 13 | 17 bytes |
| `STATUS` | 26 bytes | 27 | 31 bytes |
| `BUNDLE` | ≤195 bytes | ≤196 | ≤200 bytes |
| `CMD_ACK` | 12 bytes | 13 | 17 bytes |

### Jetson → Feather: TIME_SYNC frame (17 bytes)

```
[0xAA][0x55][len=13][PKT_TIME_SYNC payload: 13 bytes][crc8]
```

The 13-byte payload is `PktHeader (5 bytes) + TimeSyncPayload (8 bytes)`.

### Jetson → Feather: ACK_SUMMARY frame (legacy, 14 bytes)

```
[0xAA][0x55][len=10][PKT_ACK_SUMMARY payload: 10 bytes][crc8]
```

This frame format is kept here for protocol reference only. Standard-packet
app reliability is now base-managed, so `SmartFiresBaseApp` rejects Jetson
`ACK_SUMMARY` frames during normal operation.

### Jetson → Feather: command frames (CMD_CALIBRATE / CMD_RESET, 12 bytes total)

```
[0xAA][0x55][len=8][command payload: 8 bytes][crc8]
```

The 8-byte payload is `PktHeader (5 bytes) + CmdCalibratePayload/CmdResetPayload (2 bytes)
+ crc8 (1 byte)`. Total frame size (2 magic + 1 len + 8 data + 1 crc8) is 12 bytes.

## Jetson-side Frame Parser (uart_receiver.py)

`FrameReceiver` implements a byte-level state machine that runs on the single
byte emitted by `ser.read(1)` each iteration:

```
WAIT_M0 → WAIT_M1 → WAIT_LEN → READ_DATA → CHECK_CRC → (yield event)
```

- `WAIT_M0 / WAIT_M1`: re-synchronise on magic bytes; any non-magic byte resets to `WAIT_M0`.
- `WAIT_LEN`: read the `len` byte; reject frames outside `[BASE_FRAME_MIN_DATA_LEN, BASE_FRAME_MAX_DATA_LEN]`.
- `READ_DATA`: accumulate `len` bytes into a buffer.
- `CHECK_CRC`: compute CRC-8/MAXIM over `[len byte] + data`; compare against the trailing byte. On mismatch, increment `crc_failures` and reset.

On a valid frame the parser extracts:
- `rssi` (signed byte at data offset 0)
- raw LoRa payload (data bytes 1 onward)
- `pkt_type`, `node_id`, `seq` from the `PktHeader` at the start of the payload

It then dispatches to the appropriate decode function (`decode_bundle`,
`decode_status`, `decode_awaken`, `decode_cmd_ack`) and yields a structured
event dict.

## Jetson-side Ingest Loop (ingest_service.py)

`run_receive()` in `ingest_service.py` drives the main receive loop:

```python
for event, receiver, ser in iter_packets(port, baud):
    # handle AWAKEN: update session, write "awaken" CSV + JSONL row
    #   (timestamp + uid_hash — marks node boot, e.g. watchdog restarts),
    #   send immediate TIME_SYNC
    # handle STATUS: log GPS, battery, heading; write CSV + JSONL
    # handle BUNDLE/FULL_STATE: expand deltas, write telemetry CSV rows
    # handle CMD_ACK: record in session
    # periodic: save packet-loss metrics
```

### Session ID and TIME_SYNC

At startup `run_receive()` generates a random 32-bit `session_id`. A daemon
thread (`_time_sync_sender`) sends `TIME_SYNC` frames periodically at
`sync_interval_s` (default 600 s). An immediate `TIME_SYNC` is also sent:
- when the first packet arrives from any node (if no sync has been sent yet)
- whenever a node sends `AWAKEN`

All writes to the serial port are serialised via `write_lock` (a
`threading.Lock`) shared between the sync thread and the main receive loop.

### ACK Ownership

The Jetson ingest loop does not generate `ACK_SUMMARY` frames for standard
telemetry. `STATUS`, `BUNDLE`, and `FULL_STATE` are forwarded upward for
logging and visualization only. Standard-packet acknowledgement is handled
entirely inside `SmartFiresBaseApp` on the Feather base station.

## Base Station Role (SmartFiresBaseApp)

On the Feather side, `SmartFiresBaseApp` is a **protocol bridge** — it does not
decode telemetry beyond what is needed for routing:

1. Receives LoRa packets from nodes via `recvfromAck()` (auto-ACKs at the link layer).
2. Assigns `node_id` from `uid_hash` on the first `AWAKEN` (`findOrCreateNodeAssignment`).
3. Wraps the LoRa payload with RSSI into a base UART frame and writes it over USB (`Serial`).
4. Reads incoming Jetson UART frames. A `TIME_SYNC` frame from the Jetson is **not**
   directly forwarded to LoRa — it's cached (`updateJetsonTimeSource()`), and the base's
   own `maybeSendPeriodicTimeSync()` later broadcasts a LoRa `TIME_SYNC` derived from that
   cached value, on its own cadence (`BaseConfig::kPeriodicTimeSyncMs`, separately from
   whenever the Jetson frame arrived). `CMD_CALIBRATE` / `CMD_RESET` frames are queued
   (`enqueuePendingCommand()`) and later flushed via a targeted, blocking
   `_radio.sendToWait(..., targetNodeId)` to the specific node — not a broadcast.
5. Rejects `ACK_SUMMARY` frames received from the Jetson (`uart_cmd_reject` /
   `uart_cmd_dropped` warnings logged, frame discarded, no LoRa action taken) — see
   "ACK Ownership" below.
6. Logs periodic health counters every `kHealthLogPeriodMs` (5 000 ms) — three
   `LOG_INFO` lines (link/TX counters, RX-by-type counters, ACK-tracking state) —
   to the same USB stream used for the Jetson binary protocol, framed separately
   via `PKT_DEBUG_LOG`/`@SFDBG` (`FramedDebugLogSink`) so the two don't collide
   on the wire.

The base station has no knowledge of bundle contents or sensor fields, but it
does own app-layer reliability tracking and `ACK_SUMMARY` emission for
standard packets.

## SessionManager (session.py)

`SessionManager` persists node identity and status across sessions in
`~/.smartfires/session.json`. It provides:

- `on_awaken(node_id, uid_hash)`: records node identity; called on every `AWAKEN` event.
- `on_status(node_id, uid_hash, status)`: stores latest heading, GPS validity, and battery data; optionally applies magnetic declination correction via `geomag` if available.
- `on_cmd_ack(node_id, uid_hash, cmd_type, status)`: appends to the command audit log.
- `snapshot()`: returns a deep copy of the current state for the CLI or external tools.

All mutations are protected by a `threading.Lock` and written atomically to disk.
