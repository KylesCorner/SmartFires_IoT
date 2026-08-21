---
name: bandwidth-scaling
description: Airtime math and node-count scaling table for the current TDMA/bundle scheme.
category: architecture
status: current
last_verified: 2026-08-17
source_refs:
  - platformio/include/telemetry/BinaryPacket.h
  - platformio/include/config/NetworkConfig.h
  - platformio/src/radio/TdmaRadioService.cpp
related_docs:
  - tdma-protocol
  - packet-reliability
---

# Bandwidth and Scaling Math (Current Scheme)

This document summarizes sensing, bundle production, TDMA service capacity, and channel occupancy for the current SmartFires node scheme.

## Current Parameters

- Sample rate: `4 Hz`
- Bundle composition: `1 reference + 14 deltas = 15 samples`
- Bundle payload (`PKT_BUNDLE`): `195 bytes` max (`BinaryPacket::kMaxBundleLoRaSize` — `PktHeader(5) + FullStatePayload(20) + n_deltas(1) + 14×DeltaPayload(12) + crc8(1)`)
- TDMA slot width: `900 ms`
- TDMA guard: `20 ms` on each edge
- Usable slot window: `860 ms`
- Multi-send budget in node radio service: up to `3 sends/slot` (`kMaxSendsPerUpdate` in `TdmaRadioService::drainTxQueue()` — a hard iteration cap applied uniformly to fresh queue items and retransmits, not specific to `PKT_BUNDLE`; layered on top of, not a substitute for, the per-send time-budget check, conservative `340 ms` budget each for bundle-class payloads)
- ACK summary payload (`PKT_ACK_SUMMARY`): `10 bytes` on LoRa (`PktHeader + AckSummaryPayload + crc8`)
- ACK summary cadence (modeling assumption, not a configured base-side rate):
  `1 summary per node every 4 seconds` (`0.25 Hz`). The base does **not** emit
  `ACK_SUMMARY` on a fixed timer — actual emission is dirty-flag-driven (a
  summary becomes eligible whenever a node's tracker changes), gated only by
  a 25 ms anti-flood floor (`BaseConfig::kAckSummaryMinIntervalMs`) and the
  base's own TDMA slot-window availability, so real cadence can be faster or
  slower depending on traffic. The figure used in this model is borrowed from
  `NetworkConfig::kExpectedAckIntervalMs` — the **node-side** assumed cadence
  used in its own retry-wait-gate formula (see PACKET_RELIABILITY.md), not an
  edge/Jetson setting (the Jetson never generates `ACK_SUMMARY` at all) and not
  a literal base emission interval. As of the 5-slot deployment that constant
  is derived from the frame period (`kFramePeriodMs` = 4 500 ms, so
  `f_ack` = 0.222 Hz); the 0.25 Hz used in the table below is the older 4 s
  figure, left in place so the occupancy columns stay comparable across
  revisions and because it is the more conservative of the two.

## Airtime Model

Using SF7/BW125/CR 4/5 with RadioHead's 4-byte over-the-air header added on top
of each LoRa payload:

- Bundle airtime (195-byte LoRa payload + 4-byte RadioHead header = 199 bytes on air): `317.696 ms`
- ACK summary airtime (10-byte LoRa payload + 4-byte RadioHead header = 14 bytes on air): `46.336 ms`

Both figures are unchanged by the `PktHeader` flags byte that grew each payload
by one. LoRa airtime is quantized to whole symbols, and at SF7/CR 4/5 one symbol
group carries 28 payload bits — the extra 8 bits fall inside the existing
`ceil()`, so neither packet crosses into another symbol group.

## Core Formulas

Let:

- $f_s$ = sample rate (Hz)
- $S_b$ = samples per bundle
- $N$ = number of nodes
- $W$ = slot width (s)
- $k$ = sends per slot (old: $k=1$, current multi-send cap: $k=3$, i.e. `kMaxSendsPerUpdate`)

Bundle production per node:

$$
r_{prod} = \frac{f_s}{S_b}
$$

With current values:

$$
r_{prod} = \frac{4}{15} = 0.2667\ \text{bundles/s}
$$

Per-node TDMA service:

$$
r_{svc} = \frac{k}{N \cdot W}
$$

For $W=0.9$ s:

$$
r_{svc,old} = \frac{1}{0.9N},\quad r_{svc,new} = \frac{3}{0.9N}
$$

Utilization ratio (stable if $\eta \le 1$):

$$
\eta = \frac{r_{prod}}{r_{svc}}
$$

Approximate steady-state drop rate when $\eta > 1$:

$$
\text{loss} \approx \frac{\eta - 1}{\eta}
$$

Offered channel occupancy estimate:

$$
\text{uplink\_occ} = N \cdot r_{prod} \cdot T_{bundle}
$$

$$
\text{ack\_occ} = N \cdot f_{ack} \cdot T_{ack}
$$

$$
\text{total\_occ} = \text{uplink\_occ} + \text{ack\_occ}
$$

with $f_{ack}=0.25$ Hz/node (modeling assumption — see note above), $T_{bundle}=317.696$ ms, $T_{ack}=46.336$ ms.

## Node Scaling Table (2–10 nodes)

**Read $N$ carefully — the table uses it two ways.** The "Frame (s)", service
and $\eta$ columns treat $N$ as the *slot* count (`NUM_SLOTS`), since a node's
service opportunity recurs once per full rotation. The occupancy columns treat
$N$ as the count of *bundle-producing nodes*, which is one fewer — the base
occupies a slot but emits no bundles. For today's deployment (4 real nodes,
`NUM_SLOTS=5`), read frame/$\eta$ off the `5` row and occupancy off the `4`
row: 4.5 s frame, $\eta=0.40$, ~38.5% total occupancy.

| Nodes | Frame (s) | Service old (1/slot) bundles/s | Service new (3/slot) bundles/s | Util old $\eta$ | Util new $\eta$ | Loss old | Loss new | Uplink occ @ offered load | ACK occ @0.25Hz | Total occ @0.25Hz |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2 | 1.8 | 0.5556 | 1.6667 | 0.48 | 0.16 | 0.0% | 0.0% | 16.9% | 2.3% | 19.3% |
| 3 | 2.7 | 0.3704 | 1.1111 | 0.72 | 0.24 | 0.0% | 0.0% | 25.4% | 3.5% | 28.9% |
| 4 | 3.6 | 0.2778 | 0.8333 | 0.96 | 0.32 | 0.0% | 0.0% | 33.9% | 4.6% | 38.5% |
| 5 | 4.5 | 0.2222 | 0.6667 | 1.20 | 0.40 | 16.7% | 0.0% | 42.4% | 5.8% | 48.2% |
| 6 | 5.4 | 0.1852 | 0.5556 | 1.44 | 0.48 | 30.6% | 0.0% | 50.8% | 7.0% | 57.8% |
| 7 | 6.3 | 0.1587 | 0.4762 | 1.68 | 0.56 | 40.5% | 0.0% | 59.3% | 8.1% | 67.4% |
| 8 | 7.2 | 0.1389 | 0.4167 | 1.92 | 0.64 | 47.9% | 0.0% | 67.8% | 9.3% | 77.0% |
| 9 | 8.1 | 0.1235 | 0.3704 | 2.16 | 0.72 | 53.7% | 0.0% | 76.2% | 10.4% | 86.7% |
| 10 | 9.0 | 0.1111 | 0.3333 | 2.40 | 0.80 | 58.3% | 0.0% | 84.7% | 11.6% | 96.3% |

## Interpretation

- The multi-send TDMA change ($k=3$, `kMaxSendsPerUpdate`) roughly triples per-node service capacity over the old $k=1$ baseline.
- At current settings, the deployed 4 real nodes (`NUM_SLOTS=5`) at 4 Hz are comfortably stable ($\eta=0.40$). The shipped node profiles sample well below 4 Hz — `SensingConfig::kTimedSamplePeriodMs` is 1 000 ms — which puts the real $\eta$ nearer 0.10.
- With $k=3$, queue utilization $\eta$ stays well under 1 (≤0.80) all the way to 10 nodes — queue overflow is not the binding constraint in this range.
- Channel occupancy, not queue stability, becomes the binding constraint at higher node counts: total offered occupancy crosses ~77% at 8 nodes and approaches ~96% at 10 nodes, leaving little margin for retransmits, AWAKEN/TIME_SYNC traffic, or clock drift before real-world loss appears.

## Practical Notes

- The table uses offered-load occupancy (what the sensing pipeline attempts to send).
- Real-world reliability may require lower ACK summary frequency at higher node counts.
- If base station LoRa forwarding/summary scheduling is constrained, practical capacity may be lower than this model.
