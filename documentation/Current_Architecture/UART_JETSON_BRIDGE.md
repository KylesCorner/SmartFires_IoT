# UART Jetson Bridge

The Feather M0 base station and the Jetson Orin Nano communicate over a
bidirectional UART link at 115 200 baud (`Serial1` on the Feather,
`/dev/ttyTHS1` on the Jetson). A lightweight framing protocol wraps LoRa
payloads for reliable transport over the serial link.

## Physical Setup

| Side | Port | Baud | Notes |
|---|---|---|---|
| Feather M0 base | `Serial1` | 115 200 | TX pin → Jetson RX; RX pin → Jetson TX |
| Jetson Orin Nano | `/dev/ttyTHS1` | 115 200 | Enable via `jetson-io.py`; disable `nvgetty` |

One-time Jetson UART setup:
```bash
sudo /opt/nvidia/jetson-io/jetson-io.py   # enable UART pin group
sudo systemctl disable nvgetty && sudo udevadm trigger
```

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
| `AWAKEN` | 9 bytes | 10 | 14 bytes |
| `STATUS` | 21 bytes | 22 | 26 bytes |
| `BUNDLE` | ≤193 bytes | ≤194 | ≤198 bytes |
| `CMD_ACK` | 11 bytes | 12 | 16 bytes |

### Jetson → Feather: TIME_SYNC frame (16 bytes)

```
[0xAA][0x55][len=12][PKT_TIME_SYNC payload: 12 bytes][crc8]
```

The 12-byte payload is `PktHeader (4 bytes) + TimeSyncPayload (8 bytes)`.

### Jetson → Feather: ACK_SUMMARY frame (13 bytes)

```
[0xAA][0x55][len=9][PKT_ACK_SUMMARY payload: 9 bytes][crc8]
```

### Jetson → Feather: command frames (CMD_CALIBRATE / CMD_RESET)

```
[0xAA][0x55][len=11][command payload: 11 bytes][crc8]
```

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
    # handle AWAKEN: update session, send immediate TIME_SYNC
    # handle STATUS: log GPS, battery, heading; write CSV + JSONL
    # handle BUNDLE/FULL_STATE: expand deltas, write telemetry CSV rows
    # handle CMD_ACK: record in session
    # periodic: send ACK_SUMMARY frames per tracked node
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

### ACK_SUMMARY Dispatch

The ingest loop maintains `ack_state` per node. After each qualifying packet
(`BUNDLE`, `FULL_STATE`, or `STATUS`) it updates the sliding-window state.
Every `ack_interval_s` seconds (default 4 s), one `ACK_SUMMARY` frame is sent
for each node that has received traffic since the last summary. See
[PACKET_RELIABILITY.md](PACKET_RELIABILITY.md) for the full ACK mechanism.

## Base Station Role (SmartFiresBaseApp)

On the Feather side, `SmartFiresBaseApp` is a **protocol bridge** — it does not
decode telemetry beyond what is needed for routing:

1. Receives LoRa packets from nodes via `recvfromAck()` (auto-ACKs at the link layer).
2. Assigns `node_id` from `uid_hash` on the first `AWAKEN` (`findOrCreateNodeAssignment`).
3. Wraps the LoRa payload with RSSI into a base UART frame and writes it to `Serial1`.
4. Reads incoming Jetson UART frames and routes:
   - `TIME_SYNC` → LoRa broadcast to `RH_BROADCAST_ADDRESS`
   - `ACK_SUMMARY` → targeted LoRa send to the addressed node
   - `CMD_CALIBRATE` / `CMD_RESET` → targeted LoRa send to the addressed node
5. Logs periodic health counters (received count, UART TX count) to the debug UART.

The base station has no knowledge of bundle contents or sensor fields.

## SessionManager (session.py)

`SessionManager` persists node identity and status across sessions in
`~/.smartfires/session.json`. It provides:

- `on_awaken(node_id, uid_hash)`: records node identity; called on every `AWAKEN` event.
- `on_status(node_id, uid_hash, status)`: stores latest heading, GPS validity, and battery data; optionally applies magnetic declination correction via `geomag` if available.
- `on_cmd_ack(node_id, uid_hash, cmd_type, status)`: appends to the command audit log.
- `snapshot()`: returns a deep copy of the current state for the CLI or external tools.

All mutations are protected by a `threading.Lock` and written atomically to disk.
