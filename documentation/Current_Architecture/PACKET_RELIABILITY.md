---
name: packet-reliability
description: StrictLinkAck vs AppLayerAckSummary reliability modes, retry gating, ACK_SUMMARY, duty-cycled-node ack deferral, and the waitPacketSent() hang risk.
category: architecture
status: current
last_verified: 2026-08-17
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

Three independent call paths reach this:

1. **`RHReliableDatagram::acknowledge()`** (vendored, protected, unreachable
   from application code) — fired automatically whenever RadioHead's
   `recvfromAck()` accepts a unicast datagram. This is what caused the field
   incident above: the node's `checkIncomingTimeSync()` received an
   `ACK_SUMMARY`, RadioHead auto-ACKed it, and the ACK's own
   `waitPacketSent()` hung. Fixed — see "The fix (node-side receive path)"
   below.
2. **`RH_RF95::send()`** itself opens with this same no-arg
   `waitPacketSent()` — "Make sure we dont interrupt an outgoing message" —
   before arming the new transmission. This means even RadioHead's
   fire-and-forget `send()`/`sendto()` isn't actually free of the hang risk:
   it doesn't wait for its *own* transmission, but it unconditionally waits
   (unbounded) for whatever transmission came *before* it. Every node send in
   `AppLayerAckSummary` mode (fresh telemetry and app-layer retries alike)
   goes through this. Confirmed in a second field incident, structurally
   distinct from the one above: a device log showed the node's radio service
   go silent for ~115 s immediately after logging three consecutive
   `retx_blocked` lines (pure bookkeeping, no radio I/O — the hang has to be
   in whichever `send()` ran on the very next cycle), and afterward the node
   had lost TDMA sync entirely and had to redo its AWAKEN/TIME_SYNC
   handshake to recover — a bigger operational hit than the ~115 s alone.
   Fixed — see "The fix (node-side send path)" below.
3. **`RHReliableDatagram::sendtoWait()`** itself, for its own outbound
   transmission, inside its own internal retry loop — not reachable from
   outside the vendored call. See "Base Station Risk" below; **this one is
   not fixed**, and can't be with a call-site change alone.

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
protected), then waits for that ACK to physically finish transmitting via
`RHGenericDriver::waitPacketSent(timeout)` — the **bounded** overload,
capped at `NetworkConfig::kAckTxWaitMs`. Bounded, not unbounded and not
absent: a missed TX-done interrupt makes the call give up after the timeout
(logged as `ack_tx_timeout`) instead of hanging, but the normal case still
waits for genuine completion before returning. This pattern already existed
on the base side (for `PKT_AWAKEN`, originally fire-and-forget — see history
note below) before this fix; the node now uses it symmetrically, and the
base's own call picked up the bounded wait too since both share the same
`RadioHeadTdmaDriver::acknowledge()` implementation.

**History note:** the first version of this fix had `acknowledge()` return
immediately after `sendto()`, with no wait at all — reasoning that since the
caller doesn't need confirmation, and an unbounded wait is what caused the
original hang, removing the wait entirely seemed safe. It wasn't: nothing
downstream then guaranteed the ACK had finished transmitting before
`TdmaRadioService::updateRxPower()`'s `sleep()` call (or, in principle, any
other radio operation) could act next — and `RH_RF95::sleep()` has no guard
against an in-flight transmission, so it could silently abort a
still-in-flight ACK. This surfaced in the field as the base station
retransmitting `ACK_SUMMARY` far more than expected, because nodes weren't
reliably completing their ACK before the radio got put back to sleep. The
bounded wait above closes that gap: the ACK is guaranteed to either finish
or definitively time out before `acknowledge()` returns, so nothing can act
on the radio mid-transmission.

### The fix (node-side send path)

`RadioHeadTdmaDriver::send()` — the fire-and-forget send used for the node's
regular telemetry and, in `AppLayerAckSummary` mode, its app-layer
retransmits — got the same treatment as `acknowledge()`, for the same two
reasons at once: it's reachable from `RH_RF95::send()`'s own no-timeout
leading `waitPacketSent()` (path 2 above), and — because it previously
wasn't waiting for anything at all — it had the identical sleep-race
exposure `acknowledge()`'s first version turned out to have.

`send()` now calls `RHGenericDriver::waitPacketSent(NetworkConfig::
kSendTxWaitMs)` after `sendto()`, logging `send_tx_timeout` if it gives up.
`kSendTxWaitMs` reuses `kBundleTxBudgetMs` (the largest existing per-slot TX
budget, sized for a ≤195-byte `BUNDLE`) rather than branching on payload
size — waiting a bit longer than strictly necessary for a small `STATUS`/
`TIME_SYNC` send costs nothing but a few extra milliseconds in the rare
timeout case; under-timing a `BUNDLE` would defeat the point. A timeout
here, same as with `acknowledge()`, doesn't necessarily mean the packet was
lost — the SX1276 may have finished transmitting despite a missed
TX-done interrupt — so `send()`'s return value still reflects only whether
RadioHead accepted the packet for transmission, not whether the bounded
wait completed within budget.

This was not covered by the receive-path fix above — `send()` and
`acknowledge()` are separate code paths, and `send()`'s exposure went
unnoticed until a second, structurally distinct field incident (see path 2
above) pointed at it directly.

`ITdmaRadioDriver::ReceivedPacket` gained a `to` field (previously
discarded) so the node can tell a broadcast receipt apart from a direct one
— acking a broadcast would make every node on the channel reply at once and
collide with each other.

### Base Station Risk — partially fixed, path 3 remains open

**The node-side fixes above cover path 1 (auto-ACK-on-receive) and path 2
(`RH_RF95::send()`'s leading wait) everywhere `send()`/`acknowledge()` are
used — including the base, since `RadioHeadTdmaDriver` is shared code. Path 3
(`RHReliableDatagram::sendtoWait()`'s own internal wait) is untouched and
remains fully live on the base station.**

The base's periodic `TIME_SYNC` broadcast uses `_radio.send()`
(`SmartFiresBaseApp.cpp:222`) — it picked up the bounded-wait fix for free,
same as every other `send()` caller, since both node and base share
`RadioHeadTdmaDriver`.

The other three packet types the base sends — `sendDirectTimeSync()`,
`sendAckSummary()`, and `sendPendingCommand()` (CMD dispatch) — all use
`_radio.sendToWait()`, which is path 3: RadioHead's `sendtoWait()` calls
`sendto()` + `waitPacketSent()` for **its own outbound transmission**, once
per retry attempt, entirely inside its own internal loop. There's no seam
to insert a bounded wait from outside without either patching vendored code
or reimplementing the retry/sequence-number/dedup bookkeeping ourselves
(that state — `_lastSequenceNumber`, `_retransmissions`, `_seenIds` — is
private to `RHReliableDatagram`, so a from-scratch reimplementation, not a
small wrapper, would be required). If the base misses a DIO0 edge while
sending any of these three packet types, **the base itself hangs
completely**, independent of node cooperation, node health, or reliability
mode.

This has not been observed in a base-station log yet, but the mechanism is
identical to the one confirmed on the node, and the base exercises this
exact code path constantly (`ACK_SUMMARY` sends roughly every few seconds
under normal operation). `documentation/Pending_Plans/WATCHDOG_TIMER.md`
already names this as Phase 2 (base watchdog), previously deferred as
"the base has not been reported hanging yet" — this analysis is a concrete,
mechanism-level reason to no longer treat that as reassurance rather than
an open question. A watchdog is the practical mitigation for path 3, not a
call-site change — unlike paths 1 and 2, which turned out to be fixable at
the call site.

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
| `reliabilityMaxAgeMs` | 30 000 ms | Pending entry dropped after 30 s regardless of attempts — MCU standby is excluded from this age, see [Duty-Cycled Nodes](#duty-cycled-nodes-timed-mode) |
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
| `expectedAckIntervalMs` | 4 500 ms (= `kFramePeriodMs`) | Expected cadence of base-side `ACK_SUMMARY` packets — derived from the frame period, since the base can only transmit in slot 0 |
| `retryWaitMultiplierPermille` | 2000 (2.0×) | Back-off multiplier applied to the expected interval |
| `retryWaitMinMs` | 4 500 ms | Floor: one full frame rotation, so a retry can never fire before the base has had one chance to ack (`static_assert`ed against `kFramePeriodMs`) |
| `retryWaitMaxMs` | 10 000 ms | Ceiling: ~2.2 frame rotations |

At current values, `retryWaitMs` evaluates to `4500 × 2.0 = 9000`, clamped into
`[4500, 10000]` → **9 000 ms** (two full frame rotations). An entry younger than
this is skipped by `pickRetransmitCandidate()` regardless of queue idleness.

Note that `retryWaitMaxMs` is now the binding constraint at only one more slot:
at `NUM_SLOTS=6` the formula would want `5400 × 2.0 = 10 800 ms` and clamp back
to 10 000, dropping the margin below two rotations. The `static_assert` on
`kRetryWaitMinMs >= kFramePeriodMs` fires first, at the same slot count, which is
the intended prompt to re-derive both bounds rather than let the gate quietly
under-wait.

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

## Duty-Cycled Nodes (Timed mode)

A `Timed` node spends most of each duty cycle in SAMD21 standby with the SX1276
asleep. Both sides of the reliability loop have to account for that, because for
`kTimedSleepMs` (35 s) out of every cycle the node simply cannot hear anything —
and the packet most affected is the window-close bundle, whose `ACK_SUMMARY`
can only be sent in a slot 0 that falls *after* the node has already gone down.

Nothing is lost to memory across standby: SRAM is retained, so `TdmaTxQueue`,
`PacketHandler`'s accumulator, and the pending window all survive byte-identical.
The problems are all timing ones.

### Node side — standby does not count as retry time

`SmartFiresNodeApp::maybeEnterTimedMcuSleep()` calls
`TdmaRadioService::notifyMcuStandby(elapsedMs)` on wake, which slides every
pending entry's `firstSentMs`/`lastSentMs` (and `_lastAckSummarySessionMs`)
forward by the measured sleep.

Without it, the sleep counts as elapsed retry time: pending timestamps are in
session-clock terms, the session clock runs through standby (see
[RTC_SUBSECOND_SLEEP](../Pending_Plans/RTC_SUBSECOND_SLEEP.md) Phase 2), and
`kTimedSleepMs` (35 s) exceeds `reliabilityMaxAgeMs` (30 s) — so **every**
unacked entry would be discarded as `max_age` on the first post-wake drain, with
no retransmit. Not a race; it would fire on every cycle.

With the shift, those entries become eligible again one `retryWaitMs` (8 s) into
the wake. In practice they should not become eligible at all: `PKT_WINDOW_BEGIN`
goes out at the top of `WarmingUp`, and `holdPendingRetriesForAckRoundTrip()`
slides the same timestamps a further `kAckRoundTripFrames` (2) frame periods so
the deferred `ACK_SUMMARY` has time to arrive first. A retransmission during
warmup now means the `WINDOW_BEGIN` was itself lost.

The hold is keyed off the marker actually reaching the air, not its enqueue — it
can wait a whole frame for the node's slot. And `drainTxQueue()` lets a queued
`WINDOW_BEGIN` preempt a due retransmit for that slot: on the first slot after a
wake both are ready at once, and retransmit normally wins, which would mean
paying for the full bundle replay the marker exists to prevent.

### Retransmissions are marked on the wire

`pickRetransmitCandidate()` ORs `PKT_FLAG_RETX` into the *outgoing copy*'s
`PktHeader::flags` and recomputes the trailing `crc8`. The stored pending payload
is deliberately left untouched, so repeated attempts are byte-identical.

The bit tells the base its previous `ACK_SUMMARY` never landed, so that send must
bypass the "nothing changed since last sent" suppression. It also gives the
Jetson a way to separate replayed samples from first-transmission ones (`retx`
CSV column).

It used to carry a second job: while the sleep signal was a flag on a
retransmittable bundle, a replayed `WINDOW_LAST` meant the exact opposite of a
fresh one, and `RETX` was how the base told them apart. Moving the signal onto
`PKT_WINDOW_END` retired that inversion.

### Base side — deferring ACK_SUMMARY across the sleep

Each `AckTracker` carries an `asleep` flag, set by `PKT_WINDOW_END` and cleared
by any other frame from that node. Because the marker is never retransmitted the
rule needs no qualification: END means asleep, anything else means awake.
`sendPendingAckSummary()` skips `asleep` trackers **while leaving `dirty` set**,
so the acknowledgement is *deferred*, not dropped: it goes out on the first slot 0
after the node is heard from again, merged with whatever that packet added to the
mask.

This matters for more than tidiness. `sendAckSummary()` uses the blocking
`sendToWait()` (`kLinkRetries` × `kLinkAckTimeoutMs` ≈ 1 s), which is longer than
the base's own 900 ms slot 0. Without the gate the base spends roughly three
consecutive slot-0 windows blocked against a node that cannot answer, delaying
`TIME_SYNC` and queued commands for every *other* node, before
`kMaxAckSummarySendAttempts` finally trips `retryHeld`.

Two supporting rules:

| Rule | Why |
|---|---|
| A `RETX` frame sets `forceResend`, bypassing the `unchangedFromLastSent` suppression for one send | The node re-asking is proof it never got the ack. Taking the "nothing changed" shortcut would clear `dirty` and leave the node retrying into silence until `reliabilityMaxAttempts`. |
| A `PKT_WINDOW_BEGIN` sets `forceResend` too, and additionally bypasses `ackSummaryMinIntervalMs` for one flush | Anything the base transmitted during the node's standby went to a switched-off radio, so the last ack must be assumed lost whatever `lastSentAckBaseSeq`/`Mask` say. And the node's retry hold is only two frame periods long — spending it on a rate limiter meant for steady-state coalescing would let the retransmission fire anyway. |
| Silence beyond `kAckSummaryNodeSilenceMs` (2 frame periods) gates the same way | Fallback for a `PKT_WINDOW_END` that was itself lost, so `asleep` never got set. A node with nothing new to say never has `dirty` set, so this can only gate a node that really stopped responding. |

`resetAckTracker()` clears `asleep` — a rebooting node is awake and about to
`AWAKEN`, and must never stay gated on a marker from before the reset.

**Not covered:** queued `CMD_CALIBRATE`/`CMD_RESET` use the same blocking send
against the same deaf node, and `kMaxPendingCommandSendAttempts` (3, one per base
window ≈ 11 s) expires well inside a 35 s standby — so operator commands aimed at
a sleeping `Timed` node are dropped rather than deferred. Gating them needs a
deferral deadline so a genuinely dead node can't hold a command slot forever;
that design is not yet done.

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
