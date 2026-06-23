---
name: link-stats-packet-plan
description: Plan to extend PKT_STATUS with lifetime retransmit/send-failure counters so post-survey analysis can correlate LoRa link quality with GPS location, with no new packet type.
category: plan-completed
status: historical
superseded_by: packet-reliability
---

# Link Stats in STATUS Packet Plan

## Purpose

Extend the existing `PKT_STATUS` packet to carry lifetime retransmission and send-failure
counters alongside the GPS position that STATUS already provides. This lets post-survey
analysis correlate LoRa link quality (retry load) directly with geographic location, with
no new packet type and no Jetson-side joins required.

## Scope

In scope:

- Extending `StatusPayload` with two `uint16_t` counter fields (4 bytes).
- A `setLinkStats()` method on `PacketHandler` to feed radio counters into STATUS encoding.
- A `retransmitCount()` public getter on `TdmaRadioService`.
- Wiring in `SmartFiresNodeApp::update()`.
- Edge receiver update in `packet.py`.
- Documentation updates for the wire format table.

Out of scope:

- Any new packet type (`PKT_LINK_STATS` approach is superseded by this plan).
- Changes to retry logic, retry timing, or counter accumulation in `TdmaRadioService`.
- Base station firmware — it relays STATUS unchanged.
- A separate send interval or build-flag toggle — the data rides STATUS unconditionally.

## Background

`PKT_STATUS` is sent every 15 minutes and already carries:

- GPS coordinates (`lat_e7`, `lon_e7`)
- Battery voltage and percentage
- DMP heading

This makes it the natural home for link health data: every STATUS packet already anchors
the node to a geographic position, so attaching retransmit counts gives the Jetson exactly
what it needs to build a retry-density map with no additional correlation step.

`TdmaRadioService` already accumulates the counters we need:

| Private field | Meaning | Public getter |
|---|---|---|
| `_sentCount` | Total fresh telemetry packets sent | `sentCount()` — already public |
| `_retransmitCount` | Total retransmission attempts | no getter yet |
| `_failedSendCount` | Total LoRa-level send failures | `failedSendCount()` — already public |

The problem is that `PacketHandler::tryEncodeStatus()` only sees a `SensorSnapshot` —
it has no path to `TdmaRadioService`. A thin `setLinkStats()` method on `PacketHandler`
solves this without mixing network-layer data into `SensorSnapshot`.

## Objectives

1. Every STATUS packet carries lifetime `retx_total` and `fail_total` counters at zero
   additional airtime cost beyond the 4-byte payload extension.
2. No new packet type, no Jetson-side join logic.
3. No changes to retry mechanics or counter accumulation.
4. Jetson CSV gains `retx_total` and `fail_total` columns; per-interval deltas are
   computed by differencing consecutive packets.

## Success Criteria

1. STATUS packets received on the Jetson contain non-zero `retx_total` values after a
   session where retransmissions are known to have occurred.
2. `retx_total` in received STATUS packets matches the `retransmit_count` logged at the
   node over the same session.
3. Wire size is 25 bytes (was 21) — confirmed via `static_assert` and Jetson log.
4. No change to STATUS send timing or GPS/battery/heading content.
5. `packet.py` decodes the new fields without error.

## Wire Format Change

### Counter design: lifetime totals, saturated at 65535

Counters are **lifetime absolute** from node boot, never reset between STATUS packets.
The Jetson computes per-interval deltas by differencing consecutive received packets.
Saturation at `uint16_t` max (65535) rather than rollover prevents the Jetson from
computing negative deltas on overflow. At realistic retransmit rates these counters will
not approach 65535 in any survey session.

Lifetime totals are resilient to STATUS packet loss: if a packet is dropped, the next
received packet's total still allows the Jetson to compute the cumulative delta from any
previously received packet.

### Updated `StatusPayload` (20 bytes, was 16)

```
flags bits: STATUS_GPS_VALID=0x01 · STATUS_BATT_VALID=0x02 · STATUS_IMU_VALID=0x04
```

| Field | Type | Encoding | Notes |
| --- | --- | --- | --- |
| `lat_e7` | `int32_t` | degrees × 1e7 | valid if STATUS_GPS_VALID |
| `lon_e7` | `int32_t` | degrees × 1e7 | valid if STATUS_GPS_VALID |
| `battery_mv` | `uint16_t` | millivolts | valid if STATUS_BATT_VALID |
| `battery_pct` | `uint8_t` | 0–100 | valid if STATUS_BATT_VALID |
| `flags` | `uint8_t` | GPS/BATT/IMU validity bits | unchanged |
| `heading_deg_x10` | `uint16_t` | heading × 10, 0–3590 | valid if STATUS_IMU_VALID |
| `heading_accuracy` | `uint16_t` | Q12 raw; ÷ 4096 for degrees | valid if STATUS_IMU_VALID |
| `retx_total` | `uint16_t` | lifetime retransmit count, saturated at 65535 | **new** |
| `fail_total` | `uint16_t` | lifetime send-failure count, saturated at 65535 | **new** |

Wire layout:
```
PKT_STATUS: [PktHeader:4][StatusPayload:20][crc8:1] = 25 bytes  (was 21)
```

UART frame (base → Jetson):
```
[0xAA][0x55][len=21][rssi:i8][PKT_STATUS payload:20][crc8:1] = 26 bytes  (was 26 → now 30)
```

Wait — recalculating UART frame:
```
[0xAA][0x55][len:u8][rssi:i8 + LoRa payload (25 bytes)] + [crc8]
len = 1 (rssi) + 25 (LoRa) = 26
frame = 2 + 1 + 26 + 1 = 30 bytes  (was 26)
```

## Implementation Plan

### Step 1: `BinaryPacket.h` — extend `StatusPayload`, update size constant

File: `platformio/include/telemetry/BinaryPacket.h`

1. Add two fields at the end of `StatusPayload`:

```cpp
struct __attribute__((packed)) StatusPayload {
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t battery_mv;
    uint8_t  battery_pct;
    uint8_t  flags;
    uint16_t heading_deg_x10;
    uint16_t heading_accuracy;
    uint16_t retx_total;      // new — lifetime retransmit count, saturated at 65535
    uint16_t fail_total;      // new — lifetime send-failure count, saturated at 65535
};
```

2. Update the `static_assert`:

```cpp
static_assert(sizeof(StatusPayload) == 20, "StatusPayload must be 20 bytes");
```

3. Update `kStatusLoRaSize`:

```cpp
static constexpr size_t kStatusLoRaSize =
    sizeof(PktHeader) + sizeof(StatusPayload) + 1;   // 25 (was 21)
```

No changes needed to `encodeStatusPayload()` or `decodeStatus()` — both take the struct
by value/reference and use `sizeof(StatusPayload)`, so they pick up the new size
automatically.

### Step 2: `TdmaRadioService.h` — add `retransmitCount()` getter

File: `platformio/include/radio/TdmaRadioService.h`

Add alongside the existing `sentCount()` and `failedSendCount()` declarations:

```cpp
uint32_t retransmitCount() const;
```

### Step 3: `TdmaRadioService.cpp` — implement getter

File: `platformio/src/radio/TdmaRadioService.cpp`

Add alongside the existing getter implementations:

```cpp
uint32_t TdmaRadioService::retransmitCount() const { return _retransmitCount; }
```

### Step 4: `PacketHandler.h` — add `setLinkStats()` and private state

File: `platformio/include/radio/PacketHandler.h`

1. Add public method declaration (after `setBundleEncodingEnabled`):

```cpp
void setLinkStats(uint32_t retxTotal, uint32_t failTotal);
```

2. Add two private fields (after `_bundleEncodingEnabled`):

```cpp
uint32_t _retxTotal  = 0;
uint32_t _failTotal  = 0;
```

### Step 5: `PacketHandler.cpp` — implement `setLinkStats()`, wire into `tryEncodeStatus()`

File: `platformio/src/radio/PacketHandler.cpp`

1. Add implementation:

```cpp
void PacketHandler::setLinkStats(uint32_t retxTotal, uint32_t failTotal) {
    _retxTotal = retxTotal;
    _failTotal = failTotal;
}
```

2. In `tryEncodeStatus()`, before `sp.flags = flags;`, add:

```cpp
sp.retx_total = (_retxTotal > 0xFFFFu)
    ? static_cast<uint16_t>(0xFFFFu)
    : static_cast<uint16_t>(_retxTotal);
sp.fail_total = (_failTotal > 0xFFFFu)
    ? static_cast<uint16_t>(0xFFFFu)
    : static_cast<uint16_t>(_failTotal);
```

3. Update the STATUS log line to include the new fields:

```cpp
LOG_INFO("packet",
         "status_encoded len=%u node=%u flags=0x%02X session_ms=%lu retx=%u fail=%u",
         static_cast<unsigned int>(_statusLen),
         static_cast<unsigned int>(_cfg.nodeId),
         static_cast<unsigned int>(sp.flags),
         static_cast<unsigned long>(snap.sessionTimeMs),
         static_cast<unsigned int>(sp.retx_total),
         static_cast<unsigned int>(sp.fail_total));
```

### Step 6: `SmartFiresNodeApp.cpp` — call `setLinkStats()` before `push()`

File: `platformio/src/app/SmartFiresNodeApp.cpp`

In `update()`, immediately before `_packetHandler.push(snap)`:

```cpp
_packetHandler.setLinkStats(_radio.retransmitCount(), _radio.failedSendCount());
```

No new state, no new methods on `SmartFiresNodeApp`.

### Step 7: Edge receiver `packet.py` — update STATUS decode

File: `edge/edge-receiver/src/smartfires_edge/packet.py`

Update the `StatusPayload` unpack format to include the two new fields.

Before (16-byte payload):
```python
STATUS_PAYLOAD_FMT  = "<iiHBBHH"   # lat, lon, batt_mv, batt_pct, flags, hdg, hdg_acc
STATUS_PAYLOAD_SIZE = struct.calcsize(STATUS_PAYLOAD_FMT)  # 16
```

After (20-byte payload):
```python
STATUS_PAYLOAD_FMT  = "<iiHBBHHHH"  # + retx_total, fail_total
STATUS_PAYLOAD_SIZE = struct.calcsize(STATUS_PAYLOAD_FMT)  # 20
```

Update `decode_status()` to extract and return `retx_total` and `fail_total` from the
unpacked tuple.

## Code Touchpoints

| File | Change |
| --- | --- |
| `platformio/include/telemetry/BinaryPacket.h` | Two new fields in `StatusPayload`, updated `static_assert` and `kStatusLoRaSize` |
| `platformio/include/radio/TdmaRadioService.h` | `retransmitCount()` declaration |
| `platformio/src/radio/TdmaRadioService.cpp` | `retransmitCount()` implementation |
| `platformio/include/radio/PacketHandler.h` | `setLinkStats()` declaration, two private fields |
| `platformio/src/radio/PacketHandler.cpp` | `setLinkStats()` implementation, saturation logic in `tryEncodeStatus()`, updated log line |
| `platformio/src/app/SmartFiresNodeApp.cpp` | One `setLinkStats()` call before `push()` |
| `edge/edge-receiver/src/smartfires_edge/packet.py` | Updated STATUS unpack format and decode return dict |
| `documentation/CLAUDE.md` | STATUS wire size updated (21 → 25 bytes), payload table updated |
| `documentation/SOFTWARE_DESIGN.md` | Same — STATUS row in packet type table |

No changes to base station firmware, `main.cpp`, `platformio.ini`, or any test fakes.

## Validation

1. Flash `feather_m0_lora_node_debug`. Wait for first STATUS cycle. Confirm node serial log:

   ```text
   [packet] status_encoded len=25 node=... flags=... session_ms=... retx=0 fail=0
   ```

   (Zero on first STATUS is expected — no transmissions have occurred yet.)

2. Let the node run through at least one retransmission event (observable via
   `[radio] retx_sent` log lines). Confirm the next STATUS shows `retx > 0`.

3. On the Jetson, confirm `decode_status()` returns `retx_total` and `fail_total`
   fields in the decoded dict, and that they appear in the CSV log.

4. Confirm STATUS `session_time_ms` and GPS coordinates are unchanged from before — no
   regression in existing fields.

5. Compute `retx_delta = retx_total[i] - retx_total[i-1]` across two consecutive STATUS
   packets and verify it matches the count of `retx_sent` log lines in the node log over
   that interval.

## Risks and Mitigations

1. Risk: existing test fakes or unit tests snapshot `kStatusLoRaSize` or `StatusPayload`
   size as a literal.
   Mitigation: grep for `kStatusLoRaSize`, `StatusPayload`, and `21` in `test/` before
   submitting. Update any hardcoded size assertions.

2. Risk: `packet.py` is also used to decode STATUS packets already logged to CSV. Old
   logs have 16-byte payloads; the new decoder expects 20 bytes.
   Mitigation: add a length check in `decode_status()` — if `len < 25` treat as legacy
   format and set `retx_total = None`, `fail_total = None`. Allows the same code to
   handle pre- and post-change logs.

3. Risk: `_retxTotal` and `_failTotal` in `PacketHandler` are snapshot at `push()` time,
   not at `tryEncodeStatus()` encode time. If `setLinkStats()` is not called before
   every `push()`, stale values propagate into STATUS.
   Mitigation: `setLinkStats()` call is unconditional and immediately precedes `push()`
   in `SmartFiresNodeApp::update()`. Document this ordering requirement in a comment at
   the call site.

## Acceptance Checklist

1. `static_assert(sizeof(StatusPayload) == 20)` passes in native build.
2. `kStatusLoRaSize == 25` confirmed in native build.
3. Node serial log shows `retx=N` in STATUS encode lines after retransmits occur.
4. Jetson CSV contains `retx_total` and `fail_total` columns.
5. Legacy STATUS log decode does not crash (backwards-compat length check in `packet.py`).
6. Wire format table in `CLAUDE.md` and `SOFTWARE_DESIGN.md` updated to 25 bytes.
