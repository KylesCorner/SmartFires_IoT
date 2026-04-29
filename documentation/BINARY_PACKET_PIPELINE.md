# Binary Packet Pipeline

## Purpose

This document describes the current implementation plan for converting the SmartFires
telemetry path from plain-text sensor output to the binary bundle-with-delta-frames
format used in the previous architecture, now integrated into the refactored class structure.

Old planning documents are in `documentation/old/`. This document reflects the current
codebase layout.

---

## Pipeline Overview

```
DutyCycleController::telemetryReady()
         │
         ▼
SmartFiresNodeApp::update()
         │   reads each ISensor* → sensor.fillSnapshot(snap)
         │   assembles SensorSnapshot { float tempC, humidityPct, windMps, ... }
         ▼
PacketHandler::push(SensorSnapshot)
         │   quantizes floats → fixed-point integers
         │   sample 0:        stored as reference FullStatePayload
         │   samples 1–14:    stored as DeltaPayload (compact deltas from reference)
         │   on sample 15:    encode PKT_BUNDLE LoRa payload → bundleReady() = true
         │                    reset (next sample becomes new reference)
         ▼
TdmaRadioService::enqueueTelemetry(payload, len)
         │   4-slot drop-oldest ring buffer (TdmaTxQueue)
         ▼
TdmaRadioService::drainTxQueue()   [called every loop via radio.update()]
         │   TdmaClock::myTurn() — TDMA slot + guard check
         │   one TX attempt per slot index
         ▼
ITdmaRadioDriver::sendToWait()
         │   RadioHeadTdmaDriver → RHReliableDatagram::sendtoWait()
         ▼
LoRa 915 MHz → base station Feather M0
```

---

## Structs and Their Roles

### `SensorSnapshot`  (`include/telemetry/SensorSnapshot.h`)

Internal currency between the sensing loop and packet assembly. Float fields in natural
units. Not a wire format — `PacketHandler` quantizes it to fixed-point before encoding.

Each `ISensor` subclass implements `fillSnapshot(SensorSnapshot&)` to write its own
fields and set the relevant bit in `sensorFlags`.

Sensor flag bits (same as old wire format):
- `WIND  = 0x01`
- `SHT31 = 0x02`
- `GPS   = 0x04`
- `IMU   = 0x08`
- `SPS30 = 0x10`

### `BinaryPacket`  (`include/telemetry/BinaryPacket.h`)

Wire format structs and encode/decode functions. Restored from the final pre-refactor
state. Key types:

| Struct | Size | Notes |
|---|---|---|
| `PktHeader` | 4 B | magic, pkt_type, node_id, seq |
| `FullStatePayload` | 20 B | reference sample; fixed-point integers |
| `DeltaPayload` | 12 B | compact deltas from reference; wind absolute |
| `StatusPayload` | 12 B | lat/lon + battery; sent every 15 minutes |
| `TimeSyncPayload` | 8 B | session_id + session_time_ms |

Max bundle LoRa payload: `4 + 20 + 1 + 14×12 = 193 bytes`.

New function `encodeBundlePayload()` outputs the raw LoRa bytes (no UART framing),
which is what `TdmaRadioService::enqueueTelemetry()` expects.

### `PacketHandler`  (`include/radio/PacketHandler.h`, `src/radio/PacketHandler.cpp`)

Owns bundle accumulation. Stateful: holds the reference frame and partial delta array.

```
Config:
  nodeId      — written into PktHeader.node_id
  maxDeltas   — default 14 (kBundleMaxDeltas)

push(SensorSnapshot) → bool
  Returns true when a complete bundle has been encoded and is waiting.
  Caller should immediately call takeBundle() to retrieve it.

takeBundle(uint8_t* buf, size_t bufSize) → uint8_t len
  Copies encoded LoRa payload into buf. Clears bundleReady flag.
  Returns 0 if nothing ready or buf too small.

bundleReady() → bool
reset()
```

---

## Current Sensor Coverage

Only SHT31 is wired end-to-end right now. Fields populated in `SensorSnapshot`:
- `tempC`, `humidityPct`, `sensorFlags |= 0x02`

All other fields default to 0. `PacketHandler` encodes them as zero in `FullStatePayload`
and zero deltas. This is valid — `sensor_flags` tells the receiver which fields are real.

---

## What Remains

### 1. TIME_SYNC binary decode in `RadioHeadTdmaDriver`

`TdmaRadioService::isTimeSyncPacket()` currently parses a placeholder text format
`"TS,<ms>"` for native testing. The real implementation must be in
`RadioHeadTdmaDriver::receive()`, which should decode `BinaryPacket::decodeTimeSync()`
and pass the session_ms to `TdmaClock::applySync()`.

The `isTimeSyncPacket()` text path can remain for native unit tests; the driver handles
the real protocol.

### 2. STATUS packet (PKT_STATUS) — implemented

`PacketHandler` encodes a `PKT_STATUS` payload carrying GPS + battery. The first
status packet is emitted on the first `push()`, then every 15 minutes.

### 4. Remaining sensors

As sensors are added to `main.cpp`, implement `fillSnapshot()` in each:

| Sensor | Flag | Fields |
|---|---|---|
| `WindSensorRevC` | `0x01` | `windMps` |
| `Pa1010dGpsSensor` | `0x04` | `latDeg`, `lonDeg` (STATUS packet) |
| `Icm20948Sensor` | `0x08` | (no fields in current FullStatePayload) |
| `Sps30Sensor` | `0x10` | `pm1_0`, `pm2_5`, `pm4_0`, `pm10` |

### 4. Delta quantization tuning

Current compact delta encoding uses mixed precision to improve bundle density:
- `dt_ticks_250ms` (uint8), `wind_cms` absolute (uint16)
- temp delta in 0.1 C (int8), humidity delta in 0.2% (int8)
- PM1.0 and PM4.0 deltas in 1.0 ug/m3 (int8)
- PM2.5 and PM10 deltas in 0.1 ug/m3 (int16)
- `flags` bitmask marks clamped values

### 5. Base station

The base station Feather (`lora_feather_base` environment) is not yet ported to the new
class structure. It still needs to be implemented to:
- Receive LoRa bundles and forward them to the Jetson over UART
- Receive TIME_SYNC UART frames from the Jetson and broadcast them over LoRa

### 6. Edge receiver (Jetson)

`edge/packet.py` must stay aligned with `BinaryPacket.h` for delta expansion.
Current wire assumptions are FullStatePayload=20 bytes, DeltaPayload=12 bytes,
and max deltas per bundle=14.

---

## Slot Sizing Reference

At the current config (N_δ=14, R=1, SF7):
- Bundle LoRa payload: 193 bytes → airtime ≈ 313 ms
- Slot width W = 900 ms, guard G = 20 ms → TX window = 860 ms
- Worst-case TX: (1+1) × (313 + 100) = 826 ms < 860 ms ✓
- Bundle period at 4 Hz sensing: 3.75 s (reference + 14 deltas)
- Frame period (4 nodes): 3600 ms → η = 0.96 → no steady-state queue loss

For scaling beyond 2 nodes see `documentation/old/TDMA_BUNDLE_SIZING.md`.
