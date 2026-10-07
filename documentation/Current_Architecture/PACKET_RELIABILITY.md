---
name: packet-reliability
description: App-layer ACK summaries, profile-scaled retry gating, duty-cycled-node ACK deferral, and fire-and-forget control recovery.
category: architecture
status: current
last_verified: 2026-10-07
source_refs:
  - platformio/include/config/NetworkProfiles.h
  - platformio/include/config/NetworkConfig.h
  - platformio/include/config/BaseConfig.h
  - platformio/include/radio/TdmaConfig.h
  - platformio/include/radio/ITdmaRadioDriver.h
  - platformio/src/radio/TdmaRadioService.cpp
  - platformio/src/platform/RadioHeadTdmaDriver.cpp
  - platformio/src/app/SmartFiresBaseApp.cpp
related_docs:
  - tdma-protocol
  - tunable-parameters
  - radio-rx-gating
  - watchdog-timer
  - duty-cycling
---

# Packet reliability

Every selected deployment profile uses `AppLayerAckSummary`. The legacy `StrictLinkAck` path remains in the type/config surface for diagnostics and compatibility, but it is not selected by the SF7/SF9/SF10/SF12 profiles.

## Mode comparison

| Behavior | StrictLinkAck | AppLayerAckSummary (current) |
|---|---|---|
| Telemetry send | RadioHead `sendToWait()` | RadioHead `send()` |
| Immediate base ACK per telemetry frame | Yes | No |
| Sender blocks for remote ACK | Yes | No |
| App pending window | Optional/configured | Eight entries |
| Completion signal | Link ACK | Base `ACK_SUMMARY` |
| Retransmission | RadioHead retry burst | Later node slot, ACK-paced |

Both modes still wait for the local radio to finish transmitting with a bounded, actual-length timeout. “Fire-and-forget” means no remote link acknowledgement, not asynchronous access to the SX1276.

## Current app-layer path

1. `PacketHandler` gives a telemetry frame a sequence number.
2. `TdmaRadioService` dequeues it only in the node's slot.
3. A first transmission is copied into the eight-entry pending window.
4. The base tracks contiguous and out-of-order sequences per node.
5. In slot 0, the base sends `ACK_SUMMARY(node_id, ack_base_seq, ack_mask)`.
6. The node removes every covered pending frame.
7. Eligible gaps are retransmitted in a later node slot with `PKT_FLAG_RETX` set.

Only `BUNDLE`, `STATUS`, and `FULL_STATE` enter the pending reliability window. `AWAKEN`, `TIME_SYNC`, window markers, commands, `CMD_ACK`, and `ACK_SUMMARY` use their own control semantics and sequence domains.

## Pending-window policy

| Setting | Current value |
|---|---:|
| TX queue depth | 8, drop oldest |
| Pending window depth | 8 |
| Maximum total attempts | 3 |
| Maximum pending age | SF7 30 s; SF9 40 s; SF10 70 s; SF12 150 s |
| Minimum retry gap | 2 s |
| Fresh-traffic holdoff | 2 s |
| Expected ACK interval | one selected-profile frame: 4.5 / 6.5 / 11 / 25 s |
| Retry wait | two frames: 9 / 13 / 22 / 50 s |

When the pending window is full, the service evicts an eligible stale/retry entry according to its bounded policy rather than growing memory. Counters record queue drops, retry attempts, failures, and acknowledgements; lifetime retransmit/fail totals later ride in STATUS.

Retransmission selection normally gets priority before fresh queue traffic, but only one retry may be attempted per TDMA slot. A queued `WINDOW_BEGIN` preempts that retry so the base first learns that a sleeping node is awake and can release its deferred acknowledgement. Fresh packets can follow if slot budget remains.

The loop caps each update at three sends. Each candidate is admitted only if its calculated selected-modem airtime plus completion margin fits before the trailing guard. The operational maximum is 15 samples for SF7/SF9/SF10 and eight samples for SF12; the wire decoder ceiling remains 15.

## ACK summary meaning

`ack_base_seq` acknowledges every sequence through that value in modulo-256 order. Bit `i` in the 16-bit `ack_mask` acknowledges `ack_base_seq + 1 + i`. The base coalesces unchanged state, paces summaries by at least 25 ms, and rotates across dirty nodes.

The base sends `ACK_SUMMARY` fire-and-forget. Its cumulative state is repeated while dirty, so loss delays acknowledgement but does not trigger a blocking RadioHead retry burst in slot 0.

## Duty-cycled acknowledgement deferral

A Timed node announces sleep with `WINDOW_END`. The base retains dirty ACK state but stops attempting summaries while the node is known asleep. `WINDOW_BEGIN` re-enables delivery. If `WINDOW_END` is lost, silence for two selected-profile frames activates the same deferral.

Losing `WINDOW_BEGIN` is recoverable: the next retransmitted telemetry frame proves the node is awake and can provoke a fresh summary. Window markers themselves are never acknowledged or retransmitted.

## Control-packet ACK rules

- Node `AWAKEN` is fire-and-forget and repeats at the profile cadence with UID/sequence-derived jitter until direct assignment arrives.
- Direct assignment `TIME_SYNC` is fire-and-forget; a missed response leaves the node unassigned, so it asks again.
- Periodic broadcast `TIME_SYNC` is fire-and-forget and cannot be link-ACKed.
- `ACK_SUMMARY` is fire-and-forget and cumulative.
- Base-to-node commands are fire-and-forget. Nodes do not link-ACK them; they return `CMD_ACK` at the application layer.
- Normal `CMD_ACK` is queued for the node's slot and does not enter the telemetry pending window. Reset ACK is sent immediately without link ACK so it precedes state flush/reboot.

Both base and node call radio receive with `autoAck=false`. Scheduled deployment paths do not manually generate RadioHead link ACKs.

## Failure and recovery behavior

- Before initial sync or after 22 minutes without fresh sync, TDMA/radio gating becomes permissive so a node cannot lock itself out of recovery.
- On stale sync, a node restores TX power to 13 dBm and returns to the selected profile's default mode: DYNAMIC for SF7, STATIC for SF9/SF10/SF12.
- A full queue drops the oldest queued item. A full pending window remains bounded and accounts for evictions/failures.
- App-layer frames expire by age or attempts even if no summary arrives.
- Base command sends retry only when the local radio refuses to accept the frame, not because the sleeping/remote node failed to link-ACK.

## Remaining risks

RadioHead's historical unbounded `waitPacketSent()` path could hang if a TX-done interrupt edge were missed. Deployment sends use bounded, actual-length completion waits and avoid scheduled `sendToWait()` transactions; the watchdog remains a final recovery layer. The higher-SF margins, retry timing, jittered simultaneous joins, loss bursts, sequence wraparound, sleeping Timed nodes, missing window markers, stale sync, and base slot boundaries still require hardware validation.
