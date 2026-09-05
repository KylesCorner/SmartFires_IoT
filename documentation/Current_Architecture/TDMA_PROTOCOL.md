---
name: tdma-protocol
description: Slot geometry, session clock, boot handshake, and TX budget for the TDMA radio layer.
category: architecture
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/include/radio/TdmaConfig.h
  - platformio/include/config/NetworkConfig.h
  - platformio/src/radio/TdmaClock.cpp
  - platformio/src/radio/TdmaRadioService.cpp
  - platformio/include/radio/ITdmaRadioDriver.h
  - platformio/src/platform/RadioHeadTdmaDriver.cpp
related_docs:
  - packet-reliability
  - bandwidth-scaling
  - tunable-parameters
  - radio-rx-gating
---

# TDMA protocol

SmartFires divides one 915 MHz raw-LoRa channel into fixed, repeating time slots. TDMA governs steady-state traffic; joining and selected reset/recovery packets have explicit out-of-band behavior.

## Current geometry

| Setting | Value |
|---|---:|
| `NUM_SLOTS` | 5 |
| Slot width | 900 ms |
| Frame period | 4,500 ms |
| Guard | 20 ms at each slot edge |
| Usable TX window | 860 ms |
| Base slot | slot 0, address/node ID 1 |
| Assignable node slots | four, node IDs beginning at 2 |
| Sync-stale timeout | 1,320,000 ms (22 min) |
| Node RX wake-ahead for slot 0 | 150 ms |

For session time `t`, the nominal slot is `(t / 900) % 5`. `TdmaClock::myTurn()` applies the leading and trailing guards, so a node may start transmission only while the position is at least 20 ms and before 880 ms.

The base and every node consume the same `NetworkConfig::kGeometry`. A `NUM_SLOTS` mismatch changes frame length and is not recoverable at runtime: the base's slot can drift across node slots, and the assignment table may be too small.

## Join before TDMA

A node cannot know its slot before it has an assigned ID. Joining therefore occurs outside normal slot gating:

1. The node hashes the SAMD21 UID and uses a temporary radio address.
2. It sends `AWAKEN(uid_hash, reset_cause, hang_zone)` every 5 seconds with link-layer acknowledgement.
3. The base acknowledges the frame, finds or creates the persistent assignment, and queues a direct `TIME_SYNC` addressed to the temporary radio address.
4. The sync header carries the assigned node ID. The node link-ACKs the direct response, applies session time, changes its radio address, and begins sensing/TDMA.

The current `AWAKEN` is 12 bytes; the decoder still accepts the old 9-byte header/payload layout. Slot 0 is never assigned to a sensor node.

## Session clock

Packets use session-relative milliseconds rather than wall-clock time. The Jetson starts a random session ID and maps session time to UTC when ingesting. Its default USB TIME_SYNC interval is 600 seconds.

The base maintains a clock continuously. It caches Jetson time when available, otherwise falls back to its local session, and broadcasts a fire-and-forget `TIME_SYNC` every 50 seconds. Nodes update their session clock on valid direct or broadcast sync.

Before the first sync and after 22 minutes without refresh, node transmit and receive gates become permissive. This costs power/channel discipline but ensures a stale node can hear recovery sync and rejoin.

## Slot traffic

Node slots carry BUNDLE, STATUS, FULL_STATE, queued `CMD_ACK`, window markers, and retransmissions. Current app-layer telemetry uses RadioHead `send()` rather than remote link ACK. The node's queue is drained only in its guarded slot while sync is fresh.

Slot 0 carries direct assignment sync, base-to-node commands, `ACK_SUMMARY`, and periodic broadcast sync, in that priority order. The base attempts one pending category per `update()` call, and subsequent loop iterations within the same slot may send more.

Commands are fire-and-forget and acknowledged later by `CMD_ACK`. `ACK_SUMMARY` and direct sync still use blocking `sendToWait()` and can exceed slot 0 in their worst case; the possible fix is deferred, so collision isolation is not absolute.

## TX budget

Before each queued send, the node estimates a conservative time budget:

| Packet class | Budget |
|---|---:|
| Maximum BUNDLE | 340 ms |
| STATUS | 120 ms |
| AWAKEN | 90 ms |
| Other/FULL_STATE | 140 ms |

The service will not start a packet if the estimate would cross the 880 ms trailing boundary. It caps each call at three sends and only one retransmission per slot. With maximum bundles, the budget allows two, not three, inside the 860 ms usable window.

In StrictLinkAck mode the compile-time slot invariant conservatively budgets one maximum bundle, a 250 ms ACK timeout, and two guards: `340 + 250 + 40 = 630 ms < 900 ms`. Current telemetry mode does not use that remote ACK wait.

## Node receiver gating

In current app-layer reliability mode, steady node uplinks do not need to listen for immediate ACKs. The SX1276 receiver sleeps outside the base window and starts waking 150 ms before slot 0. That margin is distinct from clock-drift guards: it covers radio wake latency and main-loop jitter from blocking sensor reads.

The receiver remains available continuously while unsynchronized/stale and while the application must drain its final Timed-window queue. Direct commands can only be received in the base's slot; their response is scheduled in the node's slot, except reset ACK as documented in the reliability reference.

## Window markers

Timed nodes emit `WINDOW_BEGIN` and `WINDOW_END` without consuming telemetry reliability sequence numbers. `WINDOW_END` carries the planned standby duration and sample count. The base uses these markers to avoid sending acknowledgement summaries to a sleeping receiver; silence provides a fallback if the end marker is lost.

Markers are deliberately fire-and-forget. Their state is advisory and recoverable, while making them reliable would couple the sleep boundary to an acknowledgement that cannot arrive until after the node sleeps.

## Scaling rule

To support `N` real nodes, set `NUM_SLOTS=N+1`, rebuild/reflash every base and node, and match the edge sniffer setting. Then recalculate:

- frame period and per-node service rate;
- ACK/retry intervals (the current 4,500 ms retry floor blocks six or more slots at compile time);
- base assignment capacity;
- receiver wake and slot-overrun behavior;
- regulatory/channel-airtime limits.

See `BANDWIDTH_SCALING.md` for the current calculation and `PACKET_RELIABILITY.md` for ACK pacing.
