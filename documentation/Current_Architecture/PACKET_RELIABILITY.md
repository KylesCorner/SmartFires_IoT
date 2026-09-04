---
name: packet-reliability
description: StrictLinkAck vs AppLayerAckSummary reliability modes, retry gating, ACK_SUMMARY, duty-cycled-node ack deferral, and the waitPacketSent() hang risk.
category: architecture
status: current
last_verified: 2026-09-04
source_refs:
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

SmartFires supports two telemetry reliability modes. Current node environments compile `AppLayerAckSummary` (mode 1); `StrictLinkAck` (mode 0) remains available for diagnostics and compatibility.

## Mode comparison

| Behavior | StrictLinkAck | AppLayerAckSummary (current) |
|---|---|---|
| Telemetry send | RadioHead `sendToWait()` | RadioHead `send()` |
| Immediate base ACK per telemetry frame | Yes | No |
| Sender blocks for remote ACK | Yes | No |
| App pending window | Optional/configured | Eight entries |
| Completion signal | Link ACK | Base `ACK_SUMMARY` |
| Retransmission | RadioHead retry burst | Later node slot, ACK-paced |

Both modes still wait for the local radio to finish transmitting with a bounded timeout. “Fire-and-forget” means no remote link acknowledgement, not asynchronous access to the SX1276.

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
| Maximum pending age | 30 s |
| Minimum retry gap | 2 s |
| Fresh-traffic holdoff | 2 s |
| Expected ACK interval | one frame = 4.5 s |
| Retry wait | clamp(`2 * 4.5 s`, 4.5–10 s) = 9 s |

When the pending window is full, the service evicts an eligible stale/retry entry according to its bounded policy rather than growing memory. Counters record queue drops, retry attempts, failures, and acknowledgements; lifetime retransmit/fail totals later ride in STATUS.

Retransmission selection normally gets priority before fresh queue traffic, but only one retry may be attempted per TDMA slot. A queued `WINDOW_BEGIN` preempts that retry so the base first learns that a sleeping node is awake and can release its deferred acknowledgement. Fresh packets can follow if slot budget remains.

The loop caps each update at three sends. A full bundle consumes a conservative 340 ms budget, so only two maximum bundles fit the 860 ms usable window even though smaller packets may reach the cap.

## ACK summary meaning

`ack_base_seq` acknowledges every sequence through that value in modulo-256 order. Bit `i` in the 16-bit `ack_mask` acknowledges `ack_base_seq + 1 + i`. The base coalesces unchanged state, paces summaries by at least 25 ms, and rotates across dirty nodes.

The base uses blocking `sendToWait()` for `ACK_SUMMARY`; the node manually sends the corresponding RadioHead ACK. This is still a known slot-overrun risk because RadioHead may wait through four 250 ms attempts. The planned fix is tracked in `BASE_SLOT_OVERRUN_FIX.md`.

## Duty-cycled acknowledgement deferral

A Timed node announces sleep with `WINDOW_END`. The base retains dirty ACK state but stops attempting summaries while the node is known asleep. `WINDOW_BEGIN` re-enables delivery. If `WINDOW_END` is lost, silence for two frames (9 seconds with current geometry) activates the same deferral so the base does not repeatedly block on an unreachable receiver.

Losing `WINDOW_BEGIN` is recoverable: the next retransmitted telemetry frame proves the node is awake and can provoke a fresh summary. Window markers themselves are never acknowledged or retransmitted.

## Control-packet ACK rules

- Node `AWAKEN` uses `sendToWait()`; the base manually link-ACKs only this node-to-base type.
- Direct assignment `TIME_SYNC` uses `sendToWait()`; the node manually link-ACKs direct sync.
- Periodic broadcast `TIME_SYNC` is fire-and-forget and cannot be link-ACKed.
- `ACK_SUMMARY` uses `sendToWait()`; the node manually link-ACKs it.
- Base-to-node commands are fire-and-forget. Nodes do not link-ACK them; they return `CMD_ACK` at the application layer.
- Normal `CMD_ACK` is queued for the node's slot and does not enter the telemetry pending window. Reset ACK is sent immediately without link ACK so it precedes state flush/reboot.

Both base and node call radio receive with `autoAck=false`; all link ACK behavior is explicit by packet type.

## Failure and recovery behavior

- Before initial sync or after 22 minutes without fresh sync, TDMA/radio gating becomes permissive so a node cannot lock itself out of recovery.
- On stale sync, a node also restores TX power to the 13 dBm DYNAMIC baseline.
- A full queue drops the oldest queued item. A full pending window remains bounded and accounts for evictions/failures.
- App-layer frames expire by age or attempts even if no summary arrives.
- Base command sends retry only when the local radio refuses to accept the frame, not because the sleeping/remote node failed to link-ACK.

## Remaining risks

RadioHead's historical unbounded `waitPacketSent()` path could hang if a TX-done interrupt edge was missed. The SmartFires driver wraps local send/ack completion with bounded waits and the firmware watchdog supplies a final recovery layer. The remaining design risk is time spent in remote-ACK `sendToWait()` on base `ACK_SUMMARY` and direct sync paths, not command delivery.

The native test suite currently has unrelated known failures. Hardware validation must cover loss bursts, sequence wraparound, a sleeping Timed node, missing window markers, stale sync, and base slot boundaries.
