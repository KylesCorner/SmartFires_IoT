# TDMA Protocol

All sensor nodes share a single 915 MHz LoRa channel. TDMA divides time into
repeating frames. Each frame contains `NUM_SLOTS` equal-width slots, one per
node. A node transmits only during its assigned slot, eliminating collisions
without any runtime negotiation.

## Slot Assignment

Slot assignment is **compile-time only**:

```
slot = (NODE_ID - 1) % NUM_SLOTS
```

`NODE_ID` is derived from the SAMD21 128-bit serial number via FNV-1a hash
at runtime (via `uid_hash` in the AWAKEN handshake). `NUM_SLOTS` is a build
flag and must be identical across all deployed node Feathers.

Mismatch in `NUM_SLOTS` causes slot collisions. The base station does not
enforce TDMA — only nodes respect slot boundaries.

## Frame and Slot Layout

```
|<─────────────────── frame = NUM_SLOTS × slotWidthMs ───────────────────>|
|  slot 0 (node 1)   |  slot 1 (node 2)   |  ...  |  slot N-1 (node N)   |
|──guard──|──TX win──|──guard──|──TX win──|
   20 ms     860 ms     20 ms     860 ms
```

> **Authoritative values:** `platformio/include/config/NetworkConfig.h`.
> For the full parameter table see [TUNABLE_PARAMETERS.md](TUNABLE_PARAMETERS.md#network-and-tdma).

| Parameter | Value | Notes |
|---|---|---|
| `slotWidthMs` | 900 ms | Fits worst-case 2×(313 ms TX + 100 ms ACK timeout) + 2×20 ms guard |
| `guardMs` | 20 ms | Covers ±50 ppm crystal drift over 22 min max sync interval |
| Usable TX window | 860 ms | `slotWidthMs − guardMs` |
| `NUM_SLOTS` (build flag) | 4 (production default) | Must match across all node Feathers |
| `syncStaleMs` | 1 320 000 ms (22 min) | After this without TIME_SYNC, node transmits unconditionally |

## Session Clock

`TdmaClock` owns the session clock. It is updated on every received
`TIME_SYNC` packet via `TdmaClock::applySync(sessionMs)`.

```
sessionNow = syncSessionMs + (millis() − syncLocalMs)
```

- `syncSessionMs`: `session_time_ms` from the most recent `TIME_SYNC` payload.
- `syncLocalMs`: local `millis()` value captured when that sync was applied.

Every sample and packet carries `session_time_ms` derived from this clock so
all records on the Jetson share a common timeline.

## Slot Turn Decision

`TdmaClock::myTurn(slotIndex)` returns true when the current session time
falls within this node's slot window. When sync is stale (22 min without
`TIME_SYNC`), `myTurn()` returns true unconditionally — the node transmits
immediately rather than going silent indefinitely.

## TX Slot Budget

`TdmaRadioService::drainTxQueue()` respects a per-slot time budget to avoid
crossing into the next node's slot. Conservative per-packet budget estimates
are used:

| Packet type | Budget estimate |
|---|---:|
| `PKT_BUNDLE` | 340 ms |
| `PKT_STATUS` | 120 ms |
| `PKT_AWAKEN` | 90 ms |
| Default / unknown | 140 ms |

Up to 3 sends are attempted per `drainTxQueue()` call. Before each send the
remaining time in the slot is checked against the budget estimate. If
insufficient time remains the packet is returned to the front of the queue and
the slot is vacated.

## TX Queue

`TdmaTxQueue` holds 4 slots. It is a **drop-oldest ring buffer**: when full
and a new packet is enqueued, the oldest entry is evicted. This ensures the
node always transmits its freshest data.

## TIME_SYNC Distribution

The base station broadcasts a 12-byte `TIME_SYNC` packet every 10 minutes
(configured via `--sync-interval` on the Jetson, default 600 s). The broadcast
destination is `RH_BROADCAST_ADDRESS (0xFF)` — fire-and-forget, no ACK.

The base also sends a direct `TIME_SYNC` immediately upon receiving an `AWAKEN`
from a node. Nodes apply the sync via `TdmaClock::applySync()` during their
receive window between TX slots.

```
TimeSyncPayload (8 bytes)
  session_id:      uint32_t  — random value set at `smartfires-edge receive` startup
  session_time_ms: uint32_t  — ms since receiver started (wraps ~49 days)
```

A change in `session_id` signals the Jetson restarted. Nodes reset their
`STATUS` timer so GPS and battery data are resent promptly.

## Boot Handshake

Nodes withhold sensing until the first successful `TIME_SYNC` is received.

```
Node boots
  → broadcasts PKT_AWAKEN every 5 s to base address (via sendToWait)
  → sensors idle

Base receives AWAKEN
  → forwards to Jetson over UART
  → sends direct TIME_SYNC back to node

Node receives TIME_SYNC
  → TdmaClock::applySync() called, hasSync() = true
  → DutyCycleController begins, sensing starts
  → every subsequent packet carries valid session_time_ms
```

## Scaling Reference

See [BANDWIDTH_SCALING.md](BANDWIDTH_SCALING.md) for the full node-count vs
queue utilization and channel occupancy tables. Summary at current parameters
(SF7/BW125/CR 4/5, 4 Hz sensing, 15-sample bundles):

- 4 nodes: utilization η ≈ 0.96 with k=2 sends/slot — stable, ~37% channel occupancy
- 8 nodes: η ≈ 0.96 with k=2 — approaching queue margin, ~75% channel occupancy
- 9–10 nodes: both queue stability and occupancy become problematic
