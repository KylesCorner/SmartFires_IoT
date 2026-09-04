---
name: bandwidth-scaling
description: Airtime math and node-count scaling table for the current TDMA/bundle scheme.
category: architecture
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/include/telemetry/BinaryPacket.h
  - platformio/include/config/NetworkConfig.h
  - platformio/src/radio/TdmaRadioService.cpp
related_docs:
  - tdma-protocol
  - packet-reliability
---

# Bandwidth and scaling

## Current inputs

| Parameter | Value |
|---|---:|
| Slots | `NUM_SLOTS=5`: base + four node slots |
| Slot width / usable window | 900 ms / 860 ms |
| Bundle | 15 samples, maximum 195 bytes |
| Conservative bundle TX budget | 340 ms |
| SensorTriggered/Continuous sample period | 750 ms |
| Timed sample period | 1,000 ms |
| Timed window | 30 samples = two full bundles per 75 s cycle |

The maximum bundle is `PktHeader(5) + FullState(20) + count(1) + 14 * Delta(12) + CRC(1) = 195 bytes`. The code permits at most three sends per update, but the 860 ms usable slot can fit only two full-bundle budgets (`2 * 340 = 680 ms`; a third would require 1,020 ms). Small packets can still reach the three-send cap across repeated calls while the slot stays open.

## Production rate versus service rate

At a 750 ms sample period, a continuously active node produces:

```text
1 / (0.75 s * 15 samples) = 0.0889 bundles/s
```

A node gets one slot per frame. Using the conservative two-full-bundle limit:

```text
service = 2 / (NUM_SLOTS * 0.9 s)
```

For the shipped five-slot geometry, service is `0.444 bundles/s`, five times the continuously active production rate. The offered-load ratio is `0.0889 / 0.444 = 0.20` per node slot. Because each node has its own slot, adding nodes lengthens every node's frame rather than sharing one service queue.

| Real nodes | `NUM_SLOTS` | Frame | Full-bundle service per node | 750 ms production/service |
|---:|---:|---:|---:|---:|
| 1 | 2 | 1.8 s | 1.111/s | 0.08 |
| 4 (current capacity) | 5 | 4.5 s | 0.444/s | 0.20 |
| 6 | 7 | 6.3 s | 0.317/s | 0.28 |
| 9 | 10 | 9.0 s | 0.222/s | 0.40 |

This table is a queue-pressure bound, not a deployment recommendation. The current retry-wait static assertion prevents `NUM_SLOTS=6` or greater until retry timing is retuned, so those rows are scaling illustrations only.

## Airtime occupancy

Using the project's recorded maximum-bundle airtime of about 317.7 ms, a continuously active 750 ms node occupies approximately:

```text
0.0889 bundles/s * 0.3177 s = 2.82% raw uplink airtime
```

Four continuously active nodes would therefore use about 11.3% for first-transmission full bundles. A Timed node averages two bundles per 75 seconds, or roughly 0.85% raw uplink airtime per node (3.4% for four nodes).

These figures exclude STATUS, window markers, `AWAKEN`, time sync, acknowledgement summaries, command traffic, retransmissions, link-layer ACKs on selected control paths, and packet headers below maximum bundle size. They are useful first-order estimates, not legal duty-cycle calculations or RF bench measurements.

## Scaling constraints beyond airtime

- `NUM_SLOTS` must match on the base and all nodes; update edge `DEFAULT_NUM_SLOTS` for sniffer alignment.
- `NetworkConfig::kRetryWaitMinMs` must cover a full frame. Its 4,500 ms value exactly matches the current five-slot frame, so adding a sixth slot trips a compile-time assertion.
- Base node-assignment capacity derives from `NUM_SLOTS - 1`.
- Queue depth and pending reliability depth are both eight. Bursty sensor-triggered operation and retries can consume them even when average airtime looks comfortable.
- Blocking base `ACK_SUMMARY` and direct `TIME_SYNC` paths can overrun slot 0; pure uplink math does not capture that known risk.
- Radio range, interference, antenna placement, spreading factor, and regulatory constraints must be validated independently.

Recalculate this page whenever slot width/count, bundle layout, sample cadence, radio modem settings, or TX-budget estimates change.
