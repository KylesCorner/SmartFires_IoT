---
name: window-marker-packets
description: Replaces the WINDOW_FIRST/WINDOW_LAST header flags with dedicated PKT_WINDOW_BEGIN/PKT_WINDOW_END frames, and makes the Timed active window run to a whole bundle boundary on a fixed wake-to-wake period, removing the retransmission of the last bundle of every duty cycle.
category: plan-pending
status: in-progress — implemented 2026-08-19, awaiting user compile/flash + bake
related_docs:
  - rtc-subsecond-sleep
  - packet-reliability
  - duty-cycling
  - tdma-protocol
  - bandwidth-scaling
---

# Window Marker Packets

## Background — the duplicate this removes

`rtc-subsecond-sleep`'s T9/T11 bounded each Timed active window with
`PKT_FLAG_WINDOW_FIRST`/`PKT_FLAG_WINDOW_LAST` bits on `PKT_BUNDLE`, and its own bake
checklist predicted the consequence: *"at most one `retx` bundle per window during warmup."*
That prediction is the problem this plan addresses.

The chain, entirely by construction and firing every cycle:

1. `PacketHandler::flushWindow()` force-encoded the partial bundle left in the accumulator and
   stamped `WINDOW_LAST` on it.
2. `SmartFiresNodeApp::maybeEnterTimedMcuSleep()` held standby off up to
   `kMaxTxDrainBeforeStandbyMs` so that bundle reached the air.
3. The base saw a fresh (non-`RETX`) `WINDOW_LAST`, marked the tracker `asleep`, and
   **deferred** the `ACK_SUMMARY` — correctly, since an ack sent into standby is a ~1 s
   blocking `sendToWait()` at a switched-off radio.
4. The node slept with that bundle unacked. `notifyMcuStandby()` deliberately kept it alive.
5. Roughly 8 s into the next warmup the retry gate opened and the node re-sent the whole
   bundle stamped `RETX` — which is what finally set `forceResend` on the base and released
   the ack.

So the last bundle of every window was transmitted twice, and the second transmission carried
no new information: it existed only to tell the base "I am awake, give me the ack you are
holding." At `kTimedActiveSampleMs = 25000` with a 1 s sample period that is one full bundle
plus a 10-sample runt per window, so roughly a third of each window's payload was duplicated,
every cycle, forever.

The design also forced an awkward inversion: because the sleep signal rode on a
retransmittable data frame, a replayed `WINDOW_LAST` had to mean *the opposite* of a fresh one
(node awake and re-asking, not node about to sleep). `PKT_FLAG_RETX` exists partly to let the
base tell them apart.

## Design

Two changes that reinforce each other.

### 1. Window edges move onto their own frames

`PKT_WINDOW_BEGIN` (0x08) and `PKT_WINDOW_END` (0x09), 17 bytes each, carrying
`WindowMarkerPayload`.

**`WINDOW_BEGIN` is the half that removes the duplicate.** The retransmission was never about
lost data — it was the only way the node could say "I'm back." A 17-byte marker says the same
thing for a twentieth of the airtime, and it goes out at the *top* of the 10 s warmup rather
than 8 s into it, on a wake where the node is otherwise silent with its radio already on. The
base treats it exactly as it treats a `RETX` frame — proof its last ack never landed — and
additionally bypasses `ackSummaryMinIntervalMs` for that one flush, since the node's retry
hold is only a couple of frame periods long.

**`WINDOW_END` earns its keep differently.** Most of the win could be had by simply not
sending the flush bundle at window close, with no new packet type at all — but then the base
has no signal that the node went down, and per T11 the deferral is why the base is not
spending ~3 slot-0 windows per sleeping node per cycle blocked on `sendToWait()`, starving
`TIME_SYNC` and commands for every *other* node. A "going down" signal fundamentally cannot
ride a data frame, because the whole point is that no data frame follows it.

The markers are deliberately outside the reliability system:

- Not entered in `TdmaRadioService`'s pending window (`isTelemetryPacketForNode()` allowlists
  `BUNDLE`/`STATUS`/`FULL_STATE`), so they are sent once and forgotten.
- **They do not consume `PktHeader::seq`.** `seq` indexes the base's ack bitmap; a
  fire-and-forget frame that burned one and was then lost would leave a hole the node can never
  fill, stalling `ackBaseSeq` for 16 sequences and inflating the Jetson's loss stats.
  `window_id` is their own counter. `isTelemetryPacketType()` on the base excludes them from
  `recordTelemetrySequence()` and the `seq20` receipt window; `ingest_service.py` excludes them
  from `PacketLossTracker`.

Losing one degrades to something no worse than the old behaviour: a lost `WINDOW_END` costs
the base some slot-0 airtime acking a node that is already asleep; a lost `WINDOW_BEGIN` costs
one retransmission to prompt the deferred ack — which is what the retired flag did on *every*
window. The `RETX` recovery path therefore stays.

`session_time_ms` is in the payload because the window edge's own instant is otherwise
unrecorded: the last bundle's final sample is taken *before* the window closes, not at the
close. Timestamping the markers lets a receiver attribute bundles to windows by time rather
than arrival order, which survives loss and the reordering a retransmission introduces.
`planned_sleep_ms` tells the base exactly when the node returns — the hook for eventually
fixing T11's "known gap" where `CMD_CALIBRATE`/`CMD_RESET` to a sleeping node are dropped
rather than deferred. `sample_count` is the direct check that the window really did land on a
bundle boundary.

### 2. The active window runs to a whole bundle

`DutyCycleController` gained `setActiveWindowHold()`, driven from
`PacketHandler::hasPartialBundle()`. Past `activeSampleMs` the window stays open while the
accumulator is mid-bundle, bounded by `activeOverrunMaxMs`.

This removes runt bundles entirely: a partial bundle spends a fresh 20-byte `FullState`
reference on a handful of samples, and it was the frame that could not be acked before
standby. `kTimedActiveSampleMs` is now **derived** as
`kTimedBundlesPerWindow × kSamplesPerBundle × kTimedSamplePeriodMs` = 2 × 15 × 1000 = 30 s,
with a `static_assert` that it is a whole number of bundles — a bare `30000` would silently
desynchronise the moment `kBundleMaxDeltas` or the sample period changed, and a desynchronised
window means every window ends on a runt again. The hold is the safety net for when a starved
sample tick (a blocking SPS30 UART read, an I2C stall) shifts the accumulator off the boundary;
past the cap `flushWindow()` still force-encodes the runt rather than lose the samples.

**Fixed wake-to-wake period.** Because the window end is now data-dependent, the cycle length
would otherwise wander. `cyclePeriodMs` (75 s) is the authority and the standby is the
remainder, measured against `_cycleStartMs` so that warmup jitter, window overrun *and* the
post-close TX drain all come out of the sleep. Floored at `minStandbyMs`. Holding the period
rather than the sleep is what keeps the base's return-time prediction meaningful and cycles
comparable across a session.

10 s warmup + 30 s window + 35 s standby = 75 s. The standby is unchanged from what shipped,
so `POWER_MEASURMENTS.md` still applies; the window grew 25 s → 30 s to reach the second
bundle boundary.

### Bug found on the way

`updateSampling()` tested the window-close condition *after* taking a sample, and
`transitionTo(CooldownSleeping)` clears `_freshSampleReady` — so the closing tick read all five
sensors and then discarded the result before the app could consume it. With `activeSampleMs` an
exact multiple of `samplePeriodMs` that was every window. The close test now runs first.

### Bug found by the new tests

`drainTxQueue()` prefers a retransmit candidate over the queue once per slot. On the first slot
after a wake both are ready at once, so the due retransmission won and `WINDOW_BEGIN` went out
*behind* it — paying for the full bundle replay the marker exists to prevent, every cycle.
`TdmaTxQueue::peekPacketType()` now lets a queued `WINDOW_BEGIN` preempt the retransmit for
that slot. Caught by `test_window_begin_holds_off_a_due_retransmit`.

## Ordering on the wire

```
wake  → WINDOW_BEGIN            (top of warmup; releases the base's deferred ack)
        [queued bundles drain]
        ...active window, whole bundles only...
close → final bundle            (enqueued when its 15th sample lands)
      → WINDOW_END              (enqueued behind it, same slot where budget allows)
        standby
```

The last frame before standby is one nobody has to acknowledge. The final bundle *is* still
unacked across the sleep — `notifyMcuStandby()` still matters — but `WINDOW_BEGIN` releases the
ack early in the next warmup, before the retry gate can fire.

## Retry-gate hold

`TdmaRadioService::holdPendingRetriesForAckRoundTrip()` slides pending timestamps forward by
`kAckRoundTripFrames` (2) frame periods once a `WINDOW_BEGIN` is actually on the air — keyed off
the send, not the enqueue, because the marker can wait a whole frame for the node's slot.

This matters more as the network grows. The wake → BEGIN → slot 0 → ack round trip is up to two
frame periods: 7.2 s at `NUM_SLOTS=4`, but 14.4 s at `NUM_SLOTS=8`, past `kRetryWaitMaxMs`
(10 s). Without the explicit hold the duplicate would quietly come back as nodes are added.
The hold delays the retry; it must never cancel it, or a lost `WINDOW_BEGIN` would strand the
bundle permanently.

## Jetson side

`window_first`/`window_last` were columns on data rows. They cannot be reproduced that way: a
streaming consumer cannot know a bundle is a window's last until the window has already closed.
`window_state.py`'s `WindowTracker` now derives the columns from the marker frames:

- `window_id` — on every telemetry row and marker row. **The one to group on**: it survives a
  lost marker and does not depend on arrival order.
- `window_first` — first telemetry row after a `WINDOW_BEGIN`.
- `window_last` — on the `window_end` row itself, the only record of the true close instant.
- `planned_sleep_ms`, `window_sample_count` — on the `window_end` row.

Markers are also written as their own CSV rows (`packet_type` = `window_begin` / `window_end`).
An `AWAKEN` resets the tracker: the node's window counter restarted, and a node that rebooted
mid-sleep never sent that window's `END`.

Continuous mode emits no markers at all, so every window column is empty there — which is the
mode `first-study` runs in, so this change does not touch the study's data.

## Flashing

Wire-format change: **node, base and the edge package must go together.** This lands on top of
the already-unflashed RTC Phase 2 / GPS-disciplined-clock Step 1 work and the base's AWAKEN
crc8 fix — flash all of it in one event rather than accumulating a fourth independent unflashed
wire change.

The base relays every received frame to the Jetson regardless of `pkt_type`, so a new node
against an old base would still surface markers on the Jetson — but the ack deferral lives on
the base, so the base reflash is mandatory for the actual win.

## Verification

`pio test -e native` — `test_packet_handler` (6 rewritten window tests),
`test_tdma_radio_service` (2 new: markers stay out of the pending window; `WINDOW_BEGIN` holds
off a due retransmit), `test_duty_cycle_controller` (6 new: closing tick no longer samples and
discards, hold extends the window, overrun cap bounds it, fixed-period sleep arithmetic, floor,
sample count).

Note `test_duty_cycle_controller` had been un-compilable (every `DutyCycleController` call site
passed 5 args to a 6-arg constructor), so it had not been running. Constructing it needed a new
`FakeAnalogReader` behind a real `BatteryMonitor`. Six *pre-existing* assertions in it now fail
— see "Open items".

On hardware, the bake should show:

- One `WINDOW_BEGIN` and one `WINDOW_END` per node per cycle in the CSV, `window_id` advancing
  by 1 with no gaps.
- `window_sample_count` = 30 every window, and `duty active_window_complete overrun_ms=0`.
- **No `retx` rows during warmup** — the headline check. The old build produced one per window.
- `base window_marker ... pkt=WINDOW_END` followed by `ack_summary_defer ... reason=window_end`,
  then `ack_summary_min_interval_bypassed reason=window_begin` on the next wake.
- `radio pending_shift reason=window_begin` once per wake.
- Cycle length holding at 75 s wake-to-wake.

## Open items

- **Six pre-existing `test_duty_cycle_controller` failures**, all in tests untouched here and
  concerning behaviour not changed here: `begin()`'s sensor begin/sleep call counts; two tests
  asserting `trigger.serviceCount` when the trigger sensor is not in the `sensors[]` array
  `serviceAllSensors()` walks; and `test_idle_sleeping_does_not_wake_before_min_sleep_...`,
  whose arithmetic assumes `sleepElapsedMs()` restarts at `IdleSleeping` when `_sleepStartMs`
  deliberately spans both sleep phases. These encode assumptions that may have intentionally
  changed — left for a decision rather than guessed at.
- `test/support/Arduino.cpp` declares `FakeSerial Serial;` while `Arduino.h` defines
  `inline FakeSerialForNative Serial;`. Stale and unbuildable if the test runner picks it up.
- Deferring `CMD_CALIBRATE`/`CMD_RESET` on the same asleep/awake state, now that `WINDOW_BEGIN`
  and `planned_sleep_ms` make it cheap. T11's known gap, still open.
- `kTimedBundlesPerWindow = 2` is a first choice, not a measured one. Raising it lengthens the
  window and lowers the marker overhead per sample.
