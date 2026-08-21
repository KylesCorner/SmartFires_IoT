---
name: base-slot-overrun-fix
description: Removes the last blocking sendToWait() from the base station's slot-0 TX paths and replaces its link-layer retry with a cheap in-slot repeat, adds a deadline-aware transmit gate so no base send can start unless it fits the remainder of the slot, and repeats PKT_WINDOW_END so a single lost marker no longer strands the base's sleep tracking.
category: plan-pending
status: draft — 2026-08-21, not implemented
related_docs:
  - window-marker-packets
  - packet-reliability
  - tdma-protocol
  - duty-cycling
  - dynamic-tx-power
---

# Base Slot Overrun Fix

## Background — what was actually observed

Field symptom: the base station sometimes misses a node's `PKT_WINDOW_END`, then tries to
deliver that node's `ACK_SUMMARY` while the node is in MCU standby with its radio off. The
attempt is retried several times, and each attempt transmits past the end of slot 0 and on
top of whichever node owns slot 1.

The lost marker and the slot overrun are **two separate defects**, and only the second one is
the TDMA violation. This matters for how they get fixed: the marker will always be lost
occasionally, so the overrun has to be made structurally impossible rather than made rare.

### Defect A — the base's belief about node sleep is single-frame-fragile

`SmartFiresNodeApp::updateWindowMarkers()` sends exactly one `PKT_WINDOW_END`, once, through
the normal TX queue. Markers never enter the reliability window and are never retransmitted
(by design — see `window-marker-packets`). One lost frame and `tracker.asleep` stays `false`
for the whole standby.

The fallback is `BaseConfig::kAckSummaryNodeSilenceMs`, which is `2 * kFramePeriodMs`. At
`NUM_SLOTS=5` that is 9000 ms, and slot 0 comes around every 4500 ms — so the base gets
**about two attempts** into a sleeping node before silence gating takes over, with
`kMaxAckSummarySendAttempts = 3` as the outer cap. Two attempts is exactly what "retries
several times" describes.

Worth noting while we are here: `lastHeardMs` only advances on received frames, and during a
normal active window bundles arrive roughly every 15 s (15 samples at a 1 s period). 9 s of
silence is therefore reached routinely *inside* a live window, so the `node_silent` branch
already fires far more often than its comment implies. It defers rather than drops, so it is
not causing loss — but it means the silence fallback is not the precision instrument it reads
as, and tightening it is not a fix.

### Defect B — `sendAckSummary()` is the last blocking send on the base

```
SmartFiresBaseApp::sendAckSummary()   →  _radio.sendToWait()
                                      →  RHReliableDatagram::sendtoWait()
                                      →  (kLinkRetries + 1) * kLinkAckTimeoutMs
                                      =  4 * 250 = 1000 ms
```

against `kSlotWidthMs = 900` (860 ms usable after two 20 ms guard bands). **One attempt at
one unreachable node overruns slot 0 by construction.** The node does not need to be asleep —
a node that browned out mid-window, or one whose ack was simply lost to a fade, costs the
same 1000 ms.

This is the identical arithmetic already written down for the command path at
`SmartFiresBaseApp.cpp:867-873`, which is why `CMD_CALIBRATE`/`CMD_RESET`/`CMD_SET_TX_POWER`
were moved to fire-and-forget `send()` plus a `PKT_CMD_ACK` in the node's own slot.
`ACK_SUMMARY` did not get the same treatment.

The second half of that comment applies unchanged too. `TdmaRadioService::checkIncomingTimeSync()`
link-ACKs `ACK_SUMMARY` at `src/radio/TdmaRadioService.cpp:874`, and an `ACK_SUMMARY` can only
ever *arrive* while the base is transmitting — by construction inside slot 0. **Every
successful ack today puts a node's radio on the air inside the base's own window.**

### Why `baseTxWindowOpen()` does not catch it

`baseTxWindowOpen()` wraps `TdmaClock::myTurn()`, which answers "is the window open *right
now*" and nothing about how much of it is left. The check happens before the send; the send
then runs for up to a second. A send starting at t=850 ms into slot 0 finishes at t=1850 ms
and consumes the whole of slot 1.

The node side already solved this: `TdmaRadioService::drainTxQueue()` computes
`remainingMs = slotEndMs - positionInSlotMs()` and compares it against
`estimateTxBudgetMs(payload, len)` before each send, deferring with a `slot_defer` log when
it will not fit (`src/radio/TdmaRadioService.cpp:528-545`). The base has no equivalent.

There is also a stale invariant. `NetworkConfig.h:221` asserts

```cpp
static_assert(kSlotWidthMs > kBundleTxBudgetMs + kLinkAckTimeoutMs + 2 * kGuardMs, ...)
```

— one TX burst plus **one** ACK timeout. Its own comment says this holds because
"TdmaRadioService checks budget before each send, so at most one bundle can start in a slot
regardless of kLinkRetries." That reasoning covers the node and was never true of the base.

---

## Design

Four changes. B1/B1b/B2 are base-side (B1 has a small node-side half) and together make the
overrun impossible; N1 is node-only and makes Defect A rare. They are independent and can land
in any order, but the flashing order below is not free — see **Flashing**.

### B1. `ACK_SUMMARY` becomes fire-and-forget

Swap `_radio.sendToWait()` for `_radio.send()` in `sendAckSummary()`, and delete the
`_driver.acknowledge()` call in the `ACK_SUMMARY` branch of
`TdmaRadioService::checkIncomingTimeSync()`.

#### Why this is safe: the ack is cumulative, not per-packet

The instinctive objection is that dropping the link-layer retry makes a lost ack
unrecoverable. It does not, and the reason is structural rather than a matter of retry budgets.

`recordTelemetrySequence()` (`SmartFiresBaseApp.cpp:752-781`) maintains a cumulative ack plus a
selective bitmap — TCP-SACK shaped:

```cpp
tracker.ackMask |= (1u << (deltaFromBase - 1u));
...
while ((tracker.ackMask & 0x01u) != 0u) {   // slide the base past every
  tracker.ackBaseSeq++;                     // contiguously-received seq
  tracker.ackMask >>= 1;
}
```

`ackBaseSeq` walks forward past everything received contiguously, and `ackMask` covers the next
16 sequence numbers. **Every `ACK_SUMMARY` the base sends re-acks seq N**, until N falls out of
that 16-wide window. An `ACK_SUMMARY` frame is therefore not a per-packet receipt that can be
lost — it is a running summary, and losing one only delays information the next one carries
anyway.

Three recovery layers, in increasing cost:

| Mechanism | Latency | Cost |
|---|---|---|
| B1b in-slot repeat (below) | immediate | ~40 ms airtime |
| **Next ack re-covers it (cumulative)** | ~7–12 s | **free** |
| `PKT_FLAG_RETX` → `forceResend` → re-ack | ~9–13.5 s | full bundle replay, ≤195 B |

*Middle row.* Any telemetry frame sets `dirty`, and both `BUNDLE` and `STATUS` qualify
(`isTelemetryPacketForNode()`, `TdmaRadioService.cpp:1016-1018`). At
`SMARTFIRES_STATUS_INTERVAL_MS = 15000` with bundles every 15 s that is a frame roughly every
7.5 s, plus up to one frame period to reach slot 0.

*Bottom row.* The retry gate is `ageMs >= clamp(kExpectedAckIntervalMs × 2.0, [4500, 10000])` =
**9000 ms**; `kReliabilityMinRetryGapMs` and the one-retx-per-slot rule space the second attempt
to roughly t=13.5 s; `kReliabilityMaxAttempts = 3` allows exactly two retransmissions before
`drop_pending reason=max_attempts`.

The bottom two rows are racing, and that race is the entire argument for B1b: the RETX gate
opens at 9 s while the natural re-ack lands somewhere around 7–12 s. Landing the *first* ack
reliably is what keeps the expensive layer from firing at all.

#### The terminal case is not data loss

If every layer fails, entry N is dropped as `max_attempts`, its pending slot is freed, and
telemetry keeps flowing. **The base has had that bundle since the first transmission** — it was
recorded, forwarded over UART and logged by the Jetson the moment it arrived. The ack was only
ever the node's permission to stop holding a copy. The Jetson dedupes on `(node_id, seq)` and
flags `retx`, so a replay costs a duplicate row, never a missing one.

The one genuine loss case is the base never hearing the bundle at all, and no acknowledgement
scheme fixes uplink loss.

This is the reframing that actually justifies B1: **`ACK_SUMMARY` does not need to be reliable,
it needs to be cheap when lost.** Today it is the exact opposite — 1000 ms of blocking in an
860 ms window, whether or not it lands.

#### What is lost, and what replaces it

`sendtoWait()` does not merely wait, it *retransmits*: up to `kLinkRetries` = 3 resends inside
that 1000 ms, so a lost ack currently gets four copies in about a second. B1 on its own would
drop that to one and push recovery onto the two slower layers — and the slowest costs a full
bundle replay, which is precisely the per-cycle expense `window-marker-packets` was written to
eliminate. Trading a slot overrun for more bundle replays would be a bad trade; B1b is what
makes it not one.

The other casualty is the delivery signal. `sendToWait()`'s return value feeds exactly one
thing: `failedSendAttempts` / `retryHeld`. After B1, `send()` returns "RadioHead accepted the
frame for transmission", so the counter can only ever trip on an unhealthy radio.

Nothing needs to replace it, because the spam bound was never the circuit breaker:

- `dirty` is set *only* by `handleTelemetryAckSummary()`, i.e. only by genuinely new telemetry.
  A dark node sends nothing, so after the one send that clears `dirty` there is no second one.
- `unchangedFromLastSent` catches the case where `dirty` is set but the ack content is
  identical.

So the natural bound is one ack per new piece of information, and the cost of a wasted one
drops from a 1000 ms blocking stall to a ~10-byte frame. Keep the two fields and re-key them
to "RadioHead refused to queue the frame" — exactly the treatment and wording
`sendPendingCommand()` already uses (`SmartFiresBaseApp.cpp:895-905`), so the file carries one
pattern rather than two.

`forceResend` should now clear on a queued send rather than a delivered one. That is the
correct semantic: it means "bypass the unchanged-suppression once", and one bypassed send is
what it is for. If that frame is lost the node re-RETXes and sets it again — and
`PKT_WINDOW_BEGIN` sets it the same way without costing a bundle replay
(`SmartFiresBaseApp.cpp:649`).

### B1b. Repeat the `ACK_SUMMARY` in-slot

Send each `ACK_SUMMARY` `BaseConfig::kAckSummaryRepeatCount` times (proposed: 2) back-to-back
inside slot 0. Two 10-byte frames is ~280 ms of budget against 860 ms usable, it never blocks,
and it puts no node on the air in the base's own window.

Against the link-layer retry it replaces:

| | `sendToWait()` (today) | blind repeat ×2 |
|---|---|---|
| copies when the ack lands | 1 | 2 |
| copies when the ack is lost | up to 4 | 2 |
| dead air per missed link ACK | 250 ms | 0 |
| node transmits inside slot 0 | yes | no |
| worst-case slot occupancy | 1000 ms — **exceeds the slot** | ~280 ms |

Less adaptive, in that the second copy goes out even when the first landed. That is the right
trade here: slot 0 is latency-constrained, not airtime-constrained, and the base is idle for
most of it. Same correlation caveat as N1 — two frames tens of ms apart are well decorrelated
against collision and interference bursts, poorly against a sustained fade. Named constant, so
it can be raised or dropped back to 1 without a code change.

### B2. Deadline-aware TX gate on every base send path

Two pieces.

**`TdmaClock::myTurnRemainingMs()`** — milliseconds left in the caller's own transmit window:

- `!hasSync() || syncStale()` → `0xFFFFFFFF` (unbounded). Mirrors `myTurn()`'s existing
  unconditional-TX fallback: with no trustworthy slot timing there is no deadline to enforce.
- not the caller's slot → `0`.
- otherwise → `(slotWidthMs - guardMs) - positionInSlotMs()`, saturating at `0`.

Note `slotEndMs = slotWidthMs - guardMs` is the same expression `drainTxQueue()` uses, so the
node and the base agree on where a slot ends.

**A shared TX-budget table.** `estimateTxBudgetMs()` currently lives in an anonymous namespace
in `TdmaRadioService.cpp` and only knows the node's four packet types. Move it to a new
`include/radio/TxBudget.h` and extend it to cover the base's types and the markers. All values
stay in `NetworkConfig.h`; the header is dispatch only.

| Packet | Size | Budget |
|---|---|---|
| `PKT_BUNDLE` | ≤195 B | `kBundleTxBudgetMs` = 340 |
| `PKT_STATUS` | 27 B | `kStatusTxBudgetMs` = 120 |
| `PKT_AWAKEN` | 12 B | `kAwakenTxBudgetMs` = 90 |
| `PKT_ACK_SUMMARY` | 10 B | `kDefaultTxBudgetMs` = 140 |
| `PKT_TIME_SYNC` | 14 B | `kDefaultTxBudgetMs` = 140 |
| `PKT_CMD_*` | 8–9 B | `kDefaultTxBudgetMs` = 140 |
| `PKT_WINDOW_BEGIN` / `_END` | 17 B | `kDefaultTxBudgetMs` = 140 |

Then `baseTxWindowOpen()` gains a `remainingMsOut`, and each of the four paths in
`maybeSendInBaseWindow()` checks its own budget against the remainder before transmitting,
logging a `base_slot_defer` line (matching the node's `slot_defer`) when it declines. A
declined send stays pending and goes out at the next slot 0 — nothing is dropped.

**Interaction with B1b:** the check is per *copy*, not per ack. `sendAckSummary()` re-checks
the remainder before each repeat and simply stops early if the slot runs out, so a partially
sent repeat is fine — one copy is the pre-B1b behaviour and the cumulative-ack property covers
the rest. Do **not** require all `kAckSummaryRepeatCount` copies to fit up front, or a late-slot
ack gets deferred a whole frame when it could have sent one copy immediately.

**This is worth doing even after B1.** `send()` is bounded but not free: it still calls
`waitPacketSent(kSendTxWaitMs)`, and `kSendTxWaitMs == kBundleTxBudgetMs == 340 ms`. It also
generalises — it protects paths that do not exist yet, instead of requiring each new base send
to remember the rule.

#### B2 forces a decision about the direct `TIME_SYNC`

`sendDirectTimeSync()` (the AWAKEN reply, `SmartFiresBaseApp.cpp:535`) is still
`sendToWait()`. Its worst case is the same 1000 ms, which does not fit in 860 ms **at any
position in the slot** — so once B2 is enforcing deadlines, that path becomes permanently
unschedulable. B2 cannot land without resolving this. Three options:

- **(a) Make it fire-and-forget too — recommended.** The app-layer retry already exists and is
  more robust than the link layer's: `SmartFiresNodeApp` re-broadcasts `PKT_AWAKEN` every
  `kAwakenIntervalMs` (5 s) until sync arrives, so a lost direct sync self-heals in 5 s. The
  node's link-ACK of it (`TdmaRadioService.cpp:848`) has the identical slot-0 problem as the
  `ACK_SUMMARY` ack. This also reaches a clean end state worth stating as an invariant: **no
  base TX path blocks on a link ACK, and no node link-ACKs anything the base sends.** The
  whole defect class disappears and `acknowledge()` is left with no callers on the node.
- **(b) Per-call retry budget.** Give the driver a way to send with `retries=0`, making the
  worst case `kDefaultTxBudgetMs + kLinkAckTimeoutMs` = 390 ms, comfortably inside 860. Keeps
  a delivery signal; keeps the node transmitting inside slot 0.
- **(c) Exempt it from the gate.** Rejected — it reintroduces exactly the overrun B2 exists to
  prevent, on the one path that runs when a node is least well-behaved.

Recommendation is (a). It is a small addition to the stated scope, so it is called out here as
the plan's one open decision rather than assumed.

### N1. Repeat `PKT_WINDOW_END`

Enqueue `PKT_WINDOW_END` `kWindowEndRepeatCount` times (proposed: 2) in
`SmartFiresNodeApp::updateWindowMarkers()`. Loss probability goes from *p* to roughly *p²* for
17 bytes of airtime.

**Why both copies belong in the same slot rather than spread across frames.** The base's next
slot 0 arrives within one frame period, so a copy sent a frame later would land *after* the
base has already taken its shot at the sleeping node — which is the thing being prevented.
Same-slot duplication is what the timing actually calls for.

Budget check for that slot: the window's final bundle (340) plus two markers (140 + 140) is
620 ms against 860 ms usable, and `drainTxQueue()`'s `kMaxSendsPerUpdate` is 3. It fits, with
240 ms to spare.

**Caveat worth stating plainly:** two frames ~40 ms apart are well decorrelated against
collision and interference bursts, and poorly decorrelated against a sustained fade. This
reduces the failure rate; it does not eliminate it. That is why B1/B2 are the actual fix —
they make the residual case cheap rather than rare. Make the count a named constant in
`SensingConfig::DutyCycle` so it can be raised or dropped to 1 without a code change.

**Second-order cost to watch.** `maybeEnterTimedMcuSleep()` holds standby off while
`_radio.queuedCount() > 0`, bounded by `kMaxTxDrainBeforeStandbyMs` = 5000 ms. If the extra
marker gets `slot_defer`red to the next frame, the node stays awake an extra
`kFramePeriodMs` = 4500 ms — which fits inside the 5000 ms drain budget, but only just, and it
comes out of standby rather than out of the 75 s cycle. Watch `tx_drain_timeout` in the bake.

**`PKT_WINDOW_BEGIN` is deliberately not duplicated.** Its loss is self-healing and cheap:
`handleTelemetryAckSummary()` clears `asleep` on any telemetry frame
(`SmartFiresBaseApp.cpp:598-602`), so the first bundle of the new window releases the deferred
ack ~15 s later. That path does not set `forceResend`, so the released ack can still hit
`unchangedFromLastSent` suppression — but the node's own `RETX` recovers from there. Revisit
only if the bake shows it mattering.

---

## Files touched

| File | Change |
|---|---|
| `include/radio/TdmaClock.h` / `src/radio/TdmaClock.cpp` | add `myTurnRemainingMs()` |
| `include/radio/TxBudget.h` | **new** — `estimateTxBudgetMs()` moved out of `TdmaRadioService.cpp`, extended to base + marker types |
| `src/radio/TdmaRadioService.cpp` | use `TxBudget.h`; drop `acknowledge()` in the `ACK_SUMMARY` branch (and the direct-`TIME_SYNC` branch under option (a)) |
| `include/app/SmartFiresBaseApp.h` | `baseTxWindowOpen()` signature; `AckTracker` field comments re-keyed |
| `src/app/SmartFiresBaseApp.cpp` | `sendAckSummary()` → `send()` + in-slot repeat; deadline checks in `maybeSendInBaseWindow()`; `sendDirectTimeSync()` per the decision above |
| `include/config/BaseConfig.h` | `kAckSummaryRepeatCount`; re-key the `kMaxAckSummarySendAttempts` comment to "radio refused to queue" |
| `include/config/SensingConfig.h` | `kWindowEndRepeatCount` |
| `src/app/SmartFiresNodeApp.cpp` | repeat `PKT_WINDOW_END` |
| `include/config/NetworkConfig.h` | update the `:221` static_assert comment; add an assert that a small link-ACKed frame fits (`kDefaultTxBudgetMs + kLinkAckTimeoutMs <= kSlotWidthMs - 2*kGuardMs`) if option (b) is chosen |

---

## Verification

### Native tests

`test_tdma_clock` — `myTurnRemainingMs()` is pure logic and easy to pin down:
- returns 0 outside the caller's slot
- returns `slotWidthMs - guardMs - posInSlot` inside it, decreasing across the slot
- saturates at 0 in the trailing guard band rather than underflowing (it is `uint32_t`
  arithmetic on a subtraction that can go negative — this is the test that matters)
- returns the unbounded sentinel pre-sync and on stale sync

`test_tdma_radio_service` — the `ACK_SUMMARY` receive path no longer calls `acknowledge()`,
and `applyAckSummary()` still runs. `FakeRadio` should already record `acknowledge()` calls;
if not, that instrumentation is part of this work.

`test_config` — the new static_assert / constant.

### The coverage gap this plan does not close

**There is no `test_smartfires_base_app` suite.** The entire ack-deferral state machine —
`asleep`, `lastHeard*`, `forceResend`, `unchangedFromLastSent`, `dirty`, the circuit breaker —
has no native coverage, and B1 and B2 both land squarely in it. Every base-side change here
will be verified only on hardware.

B1 sharpens this from "untested code" into "untested code the design now depends on."
`recordTelemetrySequence()`'s cumulative-ack behaviour is the *reason* B1 is safe, and it has
no test at all — in particular the `deltaFromBase > 16` branch, which abandons the old base
seq wholesale (`ackBaseSeq = seq; ackMask = 0`) and would silently stop re-acking anything
older. Today's pending window is 8 deep with a 30 s max age, so entries expire long before 16
seqs pass and the branch should be unreachable in practice. "Should be unreachable" is exactly
the kind of claim that deserves a test, and it is a pure function of `(tracker, seq)` — the
cheapest possible thing to cover once a suite exists.

That is a pre-existing gap, not one this plan creates, and closing it is a larger piece of
work than the fix itself (it needs a `Stream` fake for the Jetson UART on top of the existing
`FakeRadio`/`FakeClock`). Flagging it rather than silently accepting it: if the bake below
turns up anything subtle in the deferral logic, that suite should be built before the next
change to this file rather than after.

### Hardware bake

Run a Timed node overnight with the base debug log captured.

1. **No overrun.** Sniffer TDMA slot/jitter stats (`sniffer_service.py`) show zero base
   transmissions outside slot 0. This is the headline check — it should hold even on cycles
   where a `WINDOW_END` was lost.
2. **The trigger still occurs, and is now harmless.** Grep for `window_marker` gaps: cycles
   where the base logged `BEGIN(n)` and `BEGIN(n+1)` with no `END(n)` between them. Confirm
   the count drops (N1 working) but expect it to be non-zero, and confirm those cycles produce
   no slot-1 collision (B1/B2 working).
3. **Ack delivery did not regress — measured as `retx` density, not ack count.** The metric
   that matters is the `retx` column density in `telemetry.csv`, because that is what a lost
   ack actually costs: a full bundle replay. It should be no worse than the pre-change
   baseline. A rise means the first ack is landing less often than the link retries were
   hiding, and B1b's repeat count is the dial — this is the one real risk in B1.

   Do **not** read `ack_summary_received` on the node as a pass/fail signal. B1b makes it go
   *up* (two copies per ack, both counted), and the cumulative-ack property means a lower count
   is not necessarily worse either. `drop_pending reason=max_attempts` in the node log is the
   better companion metric: it should stay at or near zero, and any occurrence should be
   correlated against whether the base actually logged the bundle (if it did, nothing was
   lost — see "the terminal case is not data loss").
4. **Deferrals are rare and self-clearing.** `base_slot_defer` should appear occasionally and
   never twice in a row for the same node. Frequent deferrals mean a budget is mis-sized.
5. **Standby did not shrink.** No new `tx_drain_timeout` warnings; `planned_sleep_ms` in
   `WINDOW_END` holds steady against the pre-change baseline (N1's second-order cost).

---

## Flashing

Node and base must both be reflashed. **The order is not symmetric — flash the base first.**

- **Base first** (base fire-and-forget, node still link-ACKs): the node emits a stray ACK
  inside slot 0 that nobody is waiting for. Mildly wasteful, briefly collision-prone,
  otherwise harmless.
- **Node first** (node stops ACKing, base still `sendToWait()`): the base now blocks the full
  1000 ms on **every** ack rather than only on unreachable nodes. Strictly worse than the bug
  being fixed.

N1 is node-only and wire-compatible in both directions — `handleWindowMarker()` is idempotent
for `END`, so an old base simply sets `asleep = true` twice.

This lands on top of an already-unflashed stack (window markers, `NUM_SLOTS=5`, dynamic TX
power, GPS-disciplined clock step 1, RTC sub-second sleep phase 2). Per `CLAUDE.md`, compile
and flash are the user's to run.

---

## Open items

1. **The `sendDirectTimeSync()` decision** — (a) fire-and-forget, or (b) per-call retry budget.
   B2 cannot land without one of them. Recommendation: (a).
2. **`kAckSummaryNodeSilenceMs` is doing less than its comment claims** (it fires routinely
   inside live windows, since bundles are ~15 s apart and the threshold is 9 s). Harmless
   today because it defers rather than drops. Not touched by this plan; worth a separate look
   once B1/B2 have removed the reason to care about it.
3. **Base app test suite** — see the coverage gap above. Raised in priority by B1: the
   cumulative-ack property is now load-bearing and wholly untested.
4. **`planned_sleep_ms` is still decoded and discarded** by `handleWindowMarker()`. Using it to
   compute an explicit wake time would make the *detected* sleep case exact instead of
   depending on a `BEGIN` arriving. Deliberately out of scope here; it is a robustness
   improvement to Defect A, and B1/B2 make Defect A much less costly.
5. **Command deferral to a sleeping node** (rehomed here 2026-08-21 from
   `rtc-subsecond-sleep`'s T11 and `window-marker-packets`' open items, since this plan is
   reworking the base's send paths anyway). Queued `CMD_CALIBRATE`/`CMD_RESET` use the same
   blocking send to the same deaf node, and `kMaxPendingCommandSendAttempts` (3, ≈11 s)
   expires well inside a 35 s standby — so operator commands to a sleeping `Timed` node are
   **dropped rather than deferred**. `WINDOW_BEGIN`/`WINDOW_END` and `planned_sleep_ms` now
   make gating them cheap: the base already knows the node is down and roughly when it
   returns. The design question is a deferral deadline, so a node that never comes back
   cannot hold a command slot forever. B1/B2 do not fix this by themselves — they remove the
   *overrun*, not the drop — but they land in the same functions, so doing both at once is
   cheaper than doing them separately.
6. **The retry gate races the natural re-ack.** `computeRetryWaitMs()` derives 9000 ms from
   `kExpectedAckIntervalMs` (= `kFramePeriodMs`, i.e. how fast the base *can* ack), with no
   reference to how soon the base will *naturally* re-ack — which is set by the node's own
   telemetry cadence, since any frame sets `dirty`. When the gate is the shorter of the two, a
   RETX fires and spends a full bundle replay to obtain an ack that a free one was already
   about to deliver. Whether that is actually happening is measurable from the bake data
   (check 3): correlate `retx_gate_open` timestamps against the next `ack_summary_received`.
   Out of scope here — it is a tuning question, and B1b reduces how often the gate is reached
   at all — but it is the natural follow-on if `retx` density stays higher than it should.
