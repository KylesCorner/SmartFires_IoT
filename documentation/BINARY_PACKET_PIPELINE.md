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
         │   samples 1–7:     stored as DeltaPayload (signed delta from reference)
         │   on sample 8:     encode PKT_BUNDLE LoRa payload → bundleReady() = true
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
| `FullStatePayload` | 24 B | reference sample; fixed-point integers |
| `DeltaPayload` | 16 B | signed deltas from reference; wind absolute |
| `GpsPayload` | 8 B | lat/lon × 1e7; sent once per session |
| `TimeSyncPayload` | 8 B | session_id + session_time_ms |

Max bundle LoRa payload: `4 + 24 + 1 + 7×16 = 141 bytes`.

New function `encodeBundlePayload()` outputs the raw LoRa bytes (no UART framing),
which is what `TdmaRadioService::enqueueTelemetry()` expects.

### `PacketHandler`  (`include/radio/PacketHandler.h`, `src/radio/PacketHandler.cpp`)

Owns bundle accumulation. Stateful: holds the reference frame and partial delta array.

```
Config:
  nodeId      — written into PktHeader.node_id
  maxDeltas   — default 7 (kBundleMaxDeltas)

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

### 2. GPS packet (PKT_GPS) — implemented

`PacketHandler` encodes a `PKT_GPS` payload on the first `push()` that carries
`sensorFlags & GPS_FLAG`. `gpsPacketReady()` / `takeGpsPacket()` drain it. Call
`resetGpsSession()` when a new `session_id` arrives from TIME_SYNC.

Still requires `Pa1010dGpsSensor::fillSnapshot()` to be implemented once that sensor
is wired into `main.cpp`.

### 3. Battery data alongside GPS packet

When the GPS packet is sent (session start / first fix), battery voltage and percent
should accompany it as session metadata. Options:
- Extend `GpsPayload` with battery fields (breaks the clean 8-byte struct).
- Define a new `PKT_SESSION_INFO` packet type that carries GPS + battery together.
- Enqueue a separate small battery packet immediately after the GPS packet.

The preferred approach is a new `PKT_SESSION_INFO` (type `0x06`) with a
`SessionInfoPayload` struct containing `lat_e7`, `lon_e7`, `battery_mv` (uint16_t),
and `battery_pct` (uint8_t). `PacketHandler` would encode this instead of the bare
`PKT_GPS` once a battery reading is available. Requires adding `batteryMv` and
`batteryPct` fields to `SensorSnapshot` and a `BatteryMonitor::fillSnapshot()` path.

### 4. Remaining sensors

As sensors are added to `main.cpp`, implement `fillSnapshot()` in each:

| Sensor | Flag | Fields |
|---|---|---|
| `WindSensorRevC` | `0x01` | `windMps` |
| `Pa1010dGpsSensor` | `0x04` | `latDeg`, `lonDeg` (GPS packet only) |
| `Icm20948Sensor` | `0x08` | (no fields in current FullStatePayload) |
| `Sps30Sensor` | `0x10` | `pm1_0`, `pm2_5`, `pm4_0`, `pm10` |

### 4. `uptime_ms` deprecation

`FullStatePayload` still carries `uptime_ms` (4 bytes). Once TIME_SYNC is guaranteed
to complete before first telemetry, `uptime_ms` is redundant with `session_time` and
can be removed — saving 4 bytes per bundle (141 → 137 bytes). Recalculate slot sizing
when this change is made (see `documentation/old/TDMA_BUNDLE_SIZING.md`).

### 5. Base station

The base station Feather (`lora_feather_base` environment) is not yet ported to the new
class structure. It still needs to be implemented to:
- Receive LoRa bundles and forward them to the Jetson over UART
- Receive TIME_SYNC UART frames from the Jetson and broadcast them over LoRa

### 6. Edge receiver (Jetson)

`edge/packet.py` must be updated to decode `PKT_BUNDLE` with delta expansion.
The old pre-refactor version handled this; it needs to be reconciled with the current
`FullStatePayload` layout (24 bytes, no lat/lon).

---

## Slot Sizing Reference

At the current config (N_δ=7, R=1, SF7):
- Bundle LoRa payload: 141 bytes → airtime ≈ 236 ms
- Slot width W = 900 ms, guard G = 20 ms → TX window = 860 ms
- Worst-case TX: (1+1) × (236 + 100) = 672 ms < 860 ms ✓
- Bundle period at 1 Hz sensing: 8 s (reference + 7 deltas)
- Frame period (2 nodes): 1800 ms → η = 1800/8000 = 0.225 → no data loss

For scaling beyond 2 nodes see `documentation/old/TDMA_BUNDLE_SIZING.md`.
