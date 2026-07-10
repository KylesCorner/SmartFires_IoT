---
name: packet-reliability
description: StrictLinkAck vs AppLayerAckSummary reliability modes, retry gating, ACK_SUMMARY, and the waitPacketSent() hang risk.
category: architecture
status: current
last_verified: 2026-07-10
source_refs:
  - platformio/include/config/NetworkConfig.h
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
---

# Packet Reliability

SmartFires uses a two-layer reliability architecture that keeps radio-link
control entirely on the Feather boards and excludes the Jetson from the
acknowledgement exchange.

## Packet Classification

| Packet | Direction | Reliability | Rationale |
|---|---|---|---|
| `AWAKEN` | Node → Base | Link-layer ACK (`sendToWait`) | Boot-critical; node must know base is alive |
| `TIME_SYNC` (periodic broadcast) | Base → Nodes | Fire-and-forget broadcast (`send()` to `RH_BROADCAST_ADDRESS`) | Periodic; next sync supersedes a missed one. Never ACKed — see below |
| `TIME_SYNC` (direct, AWAKEN-triggered) | Base → Node | Link-layer ACK (`sendToWait`) | `sendDirectTimeSync()` is a distinct unicast path from the periodic broadcast above — replies to one node's `AWAKEN` |
| `BUNDLE` / `STATUS` | Node → Base | Configurable (see below) | Telemetry — governed by reliability mode |
| `ACK_SUMMARY` | Base → Node | Link-layer ACK (`sendToWait`) | `SmartFiresBaseApp::sendAckSummary()` blocks on the link ACK, it is not fire-and-forget. A missed reception triggers RadioHead's own link-layer retry (`kLinkRetries`/`kLinkAckTimeoutMs`, independent of `TdmaConfig::reliabilityMode`) immediately — this is what surfaces as base-side retries if a node's Rx happens to be asleep when the base transmits; see [radio-rx-gating](../Pending_Plans/RADIO_RX_GATING.md) |
| `CMD_CALIBRATE` / `CMD_RESET` | Base → Node | Link-layer ACK (`sendToWait`) | `SmartFiresBaseApp::sendPendingCommand()` blocks on the link ACK; gives up after `BaseConfig::kMaxPendingCommandSendAttempts` and drops the command (`reason=no_link_ack`) if it never arrives |

**All four link-ACKed packet types above depend on the receiving node actually
sending that ACK back** — see "waitPacketSent() has no timeout" below for how
that ACK is generated on the node side, and why it isn't as simple as letting
RadioHead handle it automatically.

## `waitPacketSent()` Has No Timeout

### The bug

RadioHead's `RHGenericDriver::waitPacketSent()` (no-arg overload) is
`while (_mode == RHModeTx) YIELD;` — it blocks until the SX1276's DIO0 pin
signals TX-done, with **no timeout**. `_mode` only leaves `RHModeTx` from the
DIO0 interrupt handler (`RH_RF95::handleInterrupt()`). If that interrupt is
ever missed, the call never returns and the single-threaded firmware hangs
completely, with no further log output and no recovery short of a power
cycle.

DIO0 is edge-triggered (`attachInterrupt(..., RISING)`,
`RH_RF95.cpp:108`) rather than level-triggered, which the library's own
source comments acknowledge as a known "slim chance of missing events" (see
`RH_RF95.cpp:153-174`). A missed edge is gone forever — there is nothing to
re-trigger it. This was confirmed in the field via a device log where the
node hung for ~115 seconds mid-cycle, immediately after logging `rx_wake`,
then recovered and resumed completely normal operation (proving it was a
genuine wait-condition stall, not memory corruption — and proving the same
call path had already succeeded hundreds of times earlier in that same
session before hitting this).

Two independent call paths reach this:

1. **`RHReliableDatagram::acknowledge()`** (vendored, protected, unreachable
   from application code) — fired automatically whenever RadioHead's
   `recvfromAck()` accepts a unicast datagram. This is what caused the field
   incident above: the node's `checkIncomingTimeSync()` received an
   `ACK_SUMMARY`, RadioHead auto-ACKed it, and the ACK's own
   `waitPacketSent()` hung.
2. **`RHReliableDatagram::sendtoWait()`** itself, for its own outbound
   transmission — completely independent of path 1, see "Base Station Risk"
   below.

### The fix (node-side receive path)

The node's `TdmaRadioService::checkIncomingTimeSync()` now calls
`_driver.receive(packet, /*autoAck=*/false)`, disabling RadioHead's
automatic ACK-on-receive entirely (matching what `SmartFiresBaseApp` already
did for its own receive path). For the three packet types that still need
an ACK for the base's `sendToWait()` to succeed (`ACK_SUMMARY`,
`CMD_CALIBRATE`/`CMD_RESET`, and the direct/unicast `TIME_SYNC` reply — the
periodic broadcast is deliberately never ACKed), the node now calls
`_driver.acknowledge(packet.from, packet.id)` explicitly.

`ITdmaRadioDriver::acknowledge()` (implemented in
`RadioHeadTdmaDriver::acknowledge()`) reconstructs the same ACK frame
RadioHead's own `acknowledge()` would send (`setHeaderId`/`setHeaderFlags`/
`sendto`, all public RadioHead primitives — only `acknowledge()` itself is
protected), but **returns immediately after queuing the transmission,
without calling `waitPacketSent()` at all.** It can't hang, at the cost of
not confirming the ACK physically finished transmitting before returning —
an acceptable trade, since the caller doesn't need that confirmation either.
This pattern already existed on the base side (for `PKT_AWAKEN`) before this
fix; the node now uses it symmetrically.

`ITdmaRadioDriver::ReceivedPacket` gained a `to` field (previously
discarded) so the node can tell a broadcast receipt apart from a direct one
— acking a broadcast would make every node on the channel reply at once and
collide with each other.

### Base Station Risk — not yet fixed

**The node-side fix above only protects against path 1 (auto-ACK-on-receive).
Path 2 is completely untouched and remains live on the base station.**

`SmartFiresBaseApp` sends three packet types via `_radio.sendToWait()`:
`sendDirectTimeSync()`, `sendAckSummary()`, and `sendPendingCommand()`
(CMD dispatch). RadioHead's `sendtoWait()` calls `sendto()` +
`waitPacketSent()` for **its own outbound transmission** before it ever gets
to waiting for the node's reply ACK — the exact same no-timeout call, on the
exact same missed-DIO0-interrupt risk, just triggered by the base
transmitting instead of the node auto-replying. Nothing about the node-side
fix changes this: if the base misses a DIO0 edge while sending any of these
three packet types, **the base itself hangs completely**, independent of
node cooperation, node health, or reliability mode.

This has not been observed in a base-station log yet, but the mechanism is
identical to the one confirmed on the node, and the base exercises this
exact code path constantly (`ACK_SUMMARY` sends roughly every few seconds
under normal operation). `documentation/Pending_Plans/WATCHDOG_TIMER.md`
already names this as Phase 2 (base watchdog), previously deferred as
"the base has not been reported hanging yet" — this analysis is a concrete,
mechanism-level reason to no longer treat that as reassurance rather than
an open question. There is no equivalent "disable the automatic path and
ack manually" fix available here, because the hang is in the base's *own*
send, not a reply to something — the base has to actually transmit
`ACK_SUMMARY`/`TIME_SYNC`/commands to do its job, and RadioHead has no
timeout-bounded variant of `sendtoWait()` short of patching vendored code.
A watchdog is the practical mitigation, not a call-site change.

## Reliability Modes

The telemetry send path has two modes, selected at build time via the
`SMARTFIRES_TDMA_RELIABILITY_MODE` compile flag. All production and debug
node environments currently build with mode `1`.

### Mode 0 — StrictLinkAck

Each telemetry packet waits for a RadioHead link-layer acknowledgement before
the TX slot is released. If the ACK is not received within `ackTimeoutMs`,
the driver retries up to `maxRetries` times before declaring failure.

| Parameter | Value |
|---|---:|
| `enableLinkAck` | `true` |
| `maxRetries` | 3 |
| `ackTimeoutMs` | 250 ms |

A failed packet (no ACK after all retries) is dropped. There is no app-layer
retransmit in this mode.

**Use case**: diagnostics, dense lab testing where packet loss needs to be
immediately visible in serial logs.

### Mode 1 — AppLayerAckSummary (current production mode)

Telemetry packets are sent fire-and-forget at the LoRa link layer (`send()`,
no per-packet ACK wait). The base still auto-ACKs via RadioHead's receive path
(`recvfromAck()`), but the node does not block on this.

Reliability is recovered at the application layer through a **pending window**
on the node and periodic **ACK_SUMMARY** frames generated locally by the base
station firmware.

## App-Layer Reliability Mechanism

### Node Side — Pending Window

After a fresh telemetry packet is sent, `TdmaRadioService` stores a copy in a
pending window. When idle TX time is available in a later slot, the node
re-sends the oldest unacknowledged entry from this window.

| Parameter | Value | Notes |
|---|---|---|
| `kMaxReliabilityWindow` | 8 | Hard cap in firmware |
| `reliabilityWindowDepth` | 8 | Effective window size (configurable) |
| `reliabilityMaxAttempts` | 3 | Pending entry dropped after this many retransmits |
| `reliabilityMaxAgeMs` | 30 000 ms | Pending entry dropped after 30 s regardless of attempts |
| `reliabilityMinRetryGapMs` | 2 000 ms | Minimum gap between retransmits of the same seq |
| `reliabilityFreshTrafficHoldoffMs` | 2 000 ms | Retransmits are suppressed for 2 s after a fresh send |

Fresh queue entries always take priority over retransmits. Retransmits only
fill slots when the TX queue is empty.

#### ACK-Paced Retry Gate

Before a pending entry becomes eligible for retransmission, `TdmaRadioService`
computes a retry-wait duration and requires that much time to have elapsed
since the entry was last sent:

```
retryWaitMs = clamp(expectedAckIntervalMs * retryWaitMultiplierPermille / 1000,
                     retryWaitMinMs, retryWaitMaxMs)
```

| Parameter | Value | Notes |
|---|---|---|
| `expectedAckIntervalMs` | 4 000 ms | Expected cadence of base-side `ACK_SUMMARY` packets |
| `retryWaitMultiplierPermille` | 2000 (2.0×) | Back-off multiplier applied to the expected interval |
| `retryWaitMinMs` | 4 500 ms | Floor: one interval + 500 ms jitter margin |
| `retryWaitMaxMs` | 10 000 ms | Ceiling: 2.5 intervals |

At current values, `retryWaitMs` evaluates to `4000 × 2.0 = 8000`, clamped into
`[4500, 10000]` → **8 000 ms**. An entry younger than this is skipped by
`pickRetransmitCandidate()` regardless of queue idleness.

`requireAckSummaryBeforeFirstRetry` (currently `false`) optionally gates a
pending entry's *first* retransmit attempt on having observed at least one
`ACK_SUMMARY` since the entry was sent, with a fallback: once the entry's age
reaches `retryWaitMaxMs`, the gate is bypassed so the entry is not stuck
forever if no `ACK_SUMMARY` ever arrives.

The pending window uses **sequence-number matching** to avoid keeping duplicate
entries. If the same seq arrives again (e.g., re-enqueued after a slot defer),
the entry is refreshed in place.

When the window is full and a new entry must be added, the entry with the most
retransmit attempts and greatest age is evicted first.

### Base Side — ACK_SUMMARY

`SmartFiresBaseApp` tracks received telemetry sequence numbers per node in a
sliding window. Whenever it receives a standard telemetry packet (`STATUS`,
`BUNDLE`, `FULL_STATE`), it updates the per-node tracker, marks the tracker
dirty, and later emits a targeted `ACK_SUMMARY` in the base TDMA slot window.

The `ACK_SUMMARY` wire format encodes this as:

```
AckSummaryPayload (4 bytes)
  node_id:      uint8_t   — target node
  ack_base_seq: uint8_t   — highest contiguous seq acknowledged
  ack_mask:     uint16_t  — bit N set means (ack_base_seq + N + 1) is acked
```

ACK summaries are paced on the base side with a small minimum flush interval
and are only sent during the base slot window. The Jetson does not generate or
forward standard-packet acknowledgements.

### Node Side — Applying ACK_SUMMARY

`TdmaRadioService::applyAckSummary()` walks the pending window and marks each
entry acknowledged if its sequence number falls within the summary's
coverage. Acknowledged entries are immediately freed from the window.

Sequence number comparison uses 8-bit modulo arithmetic:

- If `(ack_base_seq − seq) < 128`: seq is at or before base → acknowledged.
- If `1 ≤ (seq − ack_base_seq) ≤ 16`: check the corresponding mask bit.

## Reliability Boundary

The Feather-to-Feather LoRa link owns reliability. The base station receives
telemetry, updates its local ACK tracker, and sends `ACK_SUMMARY` packets back
to the node. The Jetson receives forwarded telemetry and is not on the
acknowledgement path for standard packets.

## History

The current design replaced an earlier approach where all telemetry used
blocking `sendToWait` for every packet. See
[Completed_Plans/NETWORK_RELIABILITY_NOTES.md](../Completed_Plans/NETWORK_RELIABILITY_NOTES.md)
and [Completed_Plans/PHASE_PROGRESS.md](../Completed_Plans/PHASE_PROGRESS.md)
for the staged migration history.
