---
name: bandwidth-scaling
description: Airtime math and node-count scaling table for the current TDMA/bundle scheme.
category: architecture
status: current
last_verified: 2026-10-07
source_refs:
  - platformio/include/telemetry/BinaryPacket.h
  - platformio/include/config/NetworkProfiles.h
  - platformio/include/config/NetworkConfig.h
  - platformio/include/radio/LoRaAirtime.h
  - platformio/src/radio/TdmaRadioService.cpp
related_docs:
  - tdma-protocol
  - packet-reliability
---

# Bandwidth and scaling

## Selected-profile inputs

`SMARTFIRES_NETWORK_PROFILE` selects the complete SF7, SF9, SF10, or SF12 operating set. Every set retains five slots (base plus four nodes), 915 MHz, CR 4/5, an eight-symbol preamble, explicit headers, payload CRC, and the 13 dBm ceiling. SF7/SF9/SF10 use 125 kHz bandwidth; SF12 uses 250 kHz.

| Profile | Slot / guards | Frame | Operational bundle | Max BUNDLE airtime | Completion margin | Continuous / Timed sample | STATUS |
|---|---:|---:|---:|---:|---:|---:|---:|
| SF7 | 900 / 20 ms | 4.5 s | 14 deltas / 15 samples / 195 B | 318 ms | 22 ms | 750 / 1,000 ms | 15 s |
| SF9 | 1,300 / 30 ms | 6.5 s | 14 / 15 / 195 B | 1,005 ms | 50 ms | 750 / 1,000 ms | 30 s |
| SF10 | 2,200 / 50 ms | 11 s | 14 / 15 / 195 B | 1,805 ms | 50 ms | 1,000 / 1,000 ms | 120 s |
| SF12 | 5,000 / 100 ms | 25 s | 7 / 8 / 111 B | 2,216 ms | 100 ms | 4,000 / 4,000 ms | 300 s |

The wire decoder still accepts up to 14 deltas and 195 application bytes for every profile. The smaller SF12 value is an operating transmit cap, not a second packet format.

## Airtime and admission

`LoRaAirtime` calculates time-on-air from the actual application length, adding RadioHead's four-byte header and the selected modem tuple. Before a node or base starts a scheduled send, it requires:

```text
remaining guarded slot time >= calculated airtime + profile completion margin
```

The same length-aware budget bounds local `waitPacketSent()`. This replaces the former packet-type estimates and prevents a legal wire-size packet from being assumed schedulable merely because it decodes. Compile-time profile checks also require the operational maximum bundle plus its margin and both guards to fit.

## Offered load

For four continuously active nodes, the provisional first-send aggregate BUNDLE/STATUS airtime is approximately 13.2% at SF7, 39.0% at SF9, 49.6% at SF10, and 28.9% at SF12. These figures exclude retries, join traffic, window markers, commands, base traffic, interference, and implementation jitter. They are sizing inputs, not measured capacity or a regulatory claim.

SF12 retains the conservative eight-sample, four-second trial cadence even though 250 kHz cuts its modeled airtime roughly in half. Its maximum operational BUNDLE leaves about 2,584 ms of raw space inside the 4.8-second guarded window before the 100 ms completion margin. This is deliberate first-trial headroom; the higher-SF timing values still require hardware characterization before tightening geometry or increasing offered load.

## Scaling constraints

- `NUM_SLOTS=5` is a compile-time tripwire and must match the selected profile's five-slot geometry.
- Base assignment capacity remains `NUM_SLOTS - 1`; changing fleet geometry requires a new internally consistent profile, not a lone slot-count edit.
- Queue and pending reliability depths remain eight. Average airtime headroom does not prevent burst pressure.
- All scheduled deployment traffic is fire-and-forget at the RadioHead link layer; `ACK_SUMMARY`, repeated sync/join behavior, and `CMD_ACK` provide application recovery.
- Every base, node, and sniffer on one carrier must use the same profile. Mixed profiles are unsupported.
- Range, interference, antennas, energy, timing margin, and regional channel-use constraints require physical validation.

Recalculate and revalidate all four definitions whenever modem settings, slot geometry, packet layout, bundle cap, or cadence changes.
