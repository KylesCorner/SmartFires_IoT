# TDMA Protocol

All sensor nodes and the base station share a single 915 MHz LoRa channel.
TDMA divides time into repeating frames. Each frame contains `NUM_SLOTS`
equal-width slots. **Slot 0 is permanently reserved for the base station** —
real node IDs are assigned starting at `kFirstNodeId = 2`
(`config/BaseConfig.h`), so node 1 (→ slot 0) is never given to a real node.
Each real node transmits only during its own assigned slot; the base
transmits only during slot 0. This eliminates collisions without any
runtime negotiation, including between base-originated broadcasts and node
telemetry.

## Slot Assignment

Slot assignment is **compile-time only**:

```
slot = (NODE_ID - 1) % NUM_SLOTS
```

`NODE_ID` is derived from the SAMD21 128-bit serial number via FNV-1a hash
at runtime (via `uid_hash` in the AWAKEN handshake) for real nodes, starting
at 2. The base station uses the reserved identity `NODE_ID = 1` (→ slot 0)
internally via its own `TdmaClock` (`SmartFiresBaseApp::_baseTdmaClock`),
self-clocked from its own session time rather than from a received
`TIME_SYNC`. `NUM_SLOTS` is a build flag and must be identical across all
deployed node Feathers — **with N real nodes, `NUM_SLOTS` must be `N + 1`**,
not `N`. The base station's own `[env:feather_m0_lora_base]` build does not
currently pass a `-DNUM_SLOTS` flag and falls back to `NetworkConfig.h`'s
default (4); keep it in sync manually if a deployment changes node
`NUM_SLOTS` away from the default.

Mismatch in `NUM_SLOTS` causes slot collisions.

## Frame and Slot Layout

```
|<─────────────────── frame = NUM_SLOTS × slotWidthMs ───────────────────────>|
|  slot 0 (base)   |  slot 1 (node 2)   |  slot 2 (node 3)  |  ...  |  slot N-1 (node N+1)  |
|──guard──|──TX win──|──guard──|──TX win──|
   20 ms     860 ms     20 ms     860 ms
```

> **Authoritative values:** `platformio/include/config/NetworkConfig.h`.
> For the full parameter table see [TUNABLE_PARAMETERS.md](TUNABLE_PARAMETERS.md#network-and-tdma).

| Parameter | Value | Notes |
|---|---|---|
| `slotWidthMs` | 900 ms | Fits worst-case bundle TX (340 ms) + link-ACK timeout (250 ms) + 2×20 ms guard |
| `guardMs` | 20 ms | Covers ±50 ppm crystal drift over 22 min max sync interval |
| Usable TX window | 860 ms | `slotWidthMs − guardMs` |
| `NUM_SLOTS` (build flag) | 4 (production default) | Must match across all node Feathers |
| `syncStaleMs` | 1 320 000 ms (22 min) | After this without TIME_SYNC, node transmits unconditionally |

Real node builds run in `AppLayerAckSummary` mode (`SMARTFIRES_TDMA_RELIABILITY_MODE=1`), where
link-layer ACK is disabled (`enableLinkAck=false`); the slot-width invariant above is a
conservative upper bound that also covers `StrictLinkAck` mode. See
[PACKET_RELIABILITY.md](PACKET_RELIABILITY.md) for the active reliability mechanism.

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

Up to 3 sends are attempted per `drainTxQueue()` call (`kMaxSendsPerUpdate = 3`,
a hard iteration cap layered on top of, not replaced by, the budget check — it
applies uniformly to fresh queue items and retransmits, not just `PKT_BUNDLE`).
Before each send the remaining time in the slot is checked against the budget
estimate. If insufficient time remains, a dequeued-from-queue packet is
re-enqueued (via `TdmaTxQueue::enqueue()` — subject to the same drop-oldest
semantics as any other enqueue, not a guaranteed lossless requeue) and the
slot is vacated for this update.

## TX Queue

`TdmaTxQueue` holds 8 entries (`NetworkConfig::kQueueDepth`, capped by
`kQueueCapacityHardCap`). It is a **drop-oldest ring buffer**: when full
and a new packet is enqueued, the oldest entry is evicted. This ensures the
node always transmits its freshest data.

## TIME_SYNC Distribution

The base station broadcasts a 13-byte `TIME_SYNC` LoRa payload every 10 minutes
(configured via `--sync-interval` on the Jetson, default 600 s). The broadcast
destination is `RH_BROADCAST_ADDRESS (0xFF)` — fire-and-forget, no ACK.

The base firmware also self-generates a direct, node-targeted `TIME_SYNC` when
it receives an `AWAKEN` from a node (queued as `pendingDirectSync`, flushed
within about one frame period once the base's own slot-0 window opens — not
sent instantly mid-frame). This is independent of, and separate from, the
Jetson's own `_send_time_sync(reason="awaken")` UART send: a `TIME_SYNC` frame
arriving on the base's USB link from the Jetson is cached
(`updateJetsonTimeSource()`), not relayed over LoRa — see
[UART_JETSON_BRIDGE.md](UART_JETSON_BRIDGE.md). Nodes apply whichever sync they
receive via `TdmaClock::applySync()` during their receive window between TX
slots.

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
  → queues a self-generated direct TIME_SYNC to that node, flushed within
    ~1 frame once the base's own slot-0 window opens

Node receives TIME_SYNC
  → TdmaClock::applySync() called, hasSync() = true
  → DutyCycleController begins, sensing starts
  → every subsequent packet carries valid session_time_ms
```

## Scaling Reference

See [BANDWIDTH_SCALING.md](BANDWIDTH_SCALING.md) for the full node-count vs
queue utilization and channel occupancy tables. Summary at current parameters
(SF7/BW125/CR 4/5, 4 Hz sensing, 15-sample bundles):

- 4 nodes: utilization η ≈ 0.32 with k=3 sends/slot — comfortably stable, ~39% channel occupancy
- 8 nodes: η ≈ 0.64 with k=3 — still queue-stable, ~77% channel occupancy (near saturation)
- 9–10 nodes: channel occupancy becomes the binding constraint even though queue utilization stays <1
