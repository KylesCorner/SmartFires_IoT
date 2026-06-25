---
name: radio-rx-gating
description: Plan to put the node's SX1276 to sleep outside the base's TDMA window, since the base never transmits elsewhere and node telemetry is fire-and-forget.
category: plan-pending
status: draft
related_docs:
  - tdma-protocol
  - packet-reliability
  - duty-cycling
  - bandwidth-scaling
---

# Radio RX Power Gating

## Background

Today a node's SX1276 sits in Rx-continuous mode for the entire TDMA frame except for the
brief moments it is actively transmitting. `TdmaRadioService::update()` calls
`checkIncomingTimeSync()` unconditionally every loop tick
(`platformio/src/radio/TdmaRadioService.cpp:159`), which loops on `_driver.available()` —
and `RadioHeadTdmaDriver::available()` (`platformio/src/platform/RadioHeadTdmaDriver.cpp:83`)
calls straight into `RHReliableDatagram::available()`, which puts/keeps the SX1276 in Rx mode.
There is no TDMA gating on receive at all, only on transmit (`myTurn()`).

Per the SX1276 datasheet, Rx-continuous draws roughly 10.3 mA versus ~0.2 µA in sleep mode —
so a node that's listening ~90% of every frame for traffic that, by protocol design, can only
ever arrive in one specific slot is burning power for no reason.

This plan covers receive-side gating only. It does not change TX timing, packet formats, or
reliability semantics.

## Goals (in priority order)

1. **Pre-sync nodes stay Rx-continuous.** A node that hasn't yet received its first
   `TIME_SYNC` has no session clock and therefore no way to compute slot timing — it must
   keep listening continuously while it broadcasts `PKT_AWAKEN` every 5 s.
2. **Confirm and protect the base-side invariant**: the base must only ever transmit
   during its own slot-0 window, so that every node-bound packet a node could possibly need
   (`TIME_SYNC`, `ACK_SUMMARY`, `CMD_CALIBRATE`/`CMD_RESET`) is guaranteed to arrive there and
   nowhere else — even if every node's Rx is off everywhere else.
3. **A node needs no Rx at all during its own TX slot.** `BUNDLE`/`STATUS` sends in the
   production reliability mode are fire-and-forget (`_driver.send()`, no ACK wait), so there
   is nothing to receive in that slot.

## Status of each goal against current code

### Goal 2 is already true — needs a safeguard, not new logic

Traced `SmartFiresBaseApp`: every base-originated send funnels through
`maybeSendInBaseWindow()` (`platformio/src/app/SmartFiresBaseApp.cpp:636`), which returns
immediately unless `baseTxWindowOpen()` (`:774`) — i.e. `_baseTdmaClock.myTurn()` — is true.
That one gate covers all four send paths it dispatches to in priority order:
`sendPendingDirectTimeSync()` (AWAKEN-triggered direct sync), `sendPendingCommand()`
(CMD_CALIBRATE/RESET), `sendPendingAckSummary()`, and `maybeSendPeriodicTimeSync()` (the
10-minute broadcast). None of the four can fire outside slot 0 today.

The one place that looked suspicious — the comment at `SmartFiresBaseApp.cpp:308` about
AWAKEN having "no slot discipline yet" — is describing the **inbound** AWAKEN's arrival
time (an unsynced node broadcasts it every 5 s at a random phase), not the base's reply.
The reply is queued (`pendingDirectSync = true`) and only flushed once `_baseTdmaClock`'s own
slot-0 window opens, exactly like every other base send.

**Action for this goal: add a regression test, not new behavior.** This invariant is exactly
the thing that node-side Rx gating will be silently relying on — if it ever regresses (e.g.
someone adds a new base→node packet type and sends it inline without going through
`maybeSendInBaseWindow()`), nodes with Rx gating enabled would simply never see it, with no
error on either side. A `native` unit test asserting "no base send happens while
`_baseTdmaClock.myTurn()` is false" (using `FakeRadio`/`FakeClock` to drive the base through
several frames with all four pending-send conditions queued at once) turns a silent-failure
mode into a loud one.

### Goal 3 is already true in production mode — confirmed, not assumed

In `TdmaRadioService::drainTxQueue()` (`platformio/src/radio/TdmaRadioService.cpp:525-541`),
`telemetryUsesLinkAck(_cfg)` is `false` whenever `reliabilityMode ==
AppLayerAckSummary` (the mode both real node environments ship with — see
`packet-reliability`). In that branch, both fresh queue sends and pending-window retransmits
go through `_driver.send()` — never `sendToWait()`. `send()` → `RHReliableDatagram::sendto()`
→ plain TX, no CAD listen (explicitly disabled, see the commented-out
`setCADTimeout` call at `RadioHeadTdmaDriver.cpp:45`), no ACK wait. So in the production
reliability mode, a node's own TX slot genuinely has zero Rx dependency.

**Caveat — `StrictLinkAck` mode (mode 0, diagnostics-only) breaks this.** In that mode
`telemetryUsesLinkAck()` is `true`, and `drainTxQueue()` calls `_driver.sendToWait()`
(`TdmaRadioService.cpp:556`), which blocks waiting for a link-layer ACK from the base — that
ACK can only be heard if Rx is active immediately after the send, inside the node's own slot.
**Decision: ship Rx gating for `AppLayerAckSummary` builds only.** `StrictLinkAck` is
lab/diagnostic-only (per `packet-reliability`'s own description) and isn't power-sensitive;
gating it correctly would mean re-deriving "own slot needs Rx after TX, base slot needs Rx,
other slots don't," which is just goal 1's pre-sync logic again in miniature. Not worth the
complexity for a mode that's never deployed in the field. Implementation should branch on
`_cfg.reliabilityMode` and simply never gate Rx when it's `StrictLinkAck` — i.e. preserve
today's continuous-Rx behavior verbatim for that build.

A second, smaller exception: `SmartFiresNodeApp::sendCmdAck()`
(`platformio/src/app/SmartFiresNodeApp.cpp:444`) sends `CMD_ACK` via
`_radio.sendImmediate(payload, len, /*requireLinkAck=*/true)` regardless of reliability mode
— this one blocking call does need Rx for its ACK. It fires only in direct response to having
just received `CMD_CALIBRATE`/`CMD_RESET`, which (per goal 2) only ever arrives in the base's
slot 0 — so this call happens essentially immediately after Rx was already open for that
reception. No special-casing needed: `sendToWait()` and `send()` are RadioHead calls that set
the SX1276's mode register themselves on every invocation, regardless of whatever sleep state
our gating logic left the radio in beforehand. An explicit `sleep()` call is advisory/idle-only
— it never blocks or interferes with a subsequent active driver call, which always re-asserts
the mode it needs. This means our gating logic can be a dumb "if window says sleep, sleep"
without worrying about racing an in-flight blocking send.

## Design

### Rx state by slot, post-sync, `AppLayerAckSummary` mode

```
slot 0 (base)         : Rx ON   — only place anything node-bound can arrive
my own slot           : Rx OFF  — TX is fire-and-forget, nothing to receive
any other node's slot : Rx OFF  — base never sends there (goal 2); other nodes
                                   never address packets to me there
```

### New query on `TdmaClock` — **implemented**

Added `TdmaClock::baseRxWindowOpen()` (`include/radio/TdmaClock.h`,
`src/radio/TdmaClock.cpp`):

- True for the *entire* slot-0 duration (`currentSlotNumber() == 0`) — deliberately no
  guard-band exclusion. `myTurn()` carves the guard off both ends of *its* slot because a
  transmitter running long risks colliding with the next slot's owner; a receiver listening a
  little extra is harmless, so the full slot stays open.
- No separate wake-ahead margin was added. The base itself never transmits before
  `posInSlot >= guardMs` into slot 0 (the same guard band `myTurn()` already enforces on the
  base's own send gating) — so a node that starts listening at `posInSlot == 0` always has at
  least the full 20 ms guard band before the base could possibly transmit. That happens to be
  enough margin "for free" without inventing a new constant. Revisit only if bench testing
  (see below) shows the SX1276's sleep→Rx wake time exceeds 20 ms on the actual RFM95 modules.
- True unconditionally when `!hasSync() || syncStale()` — same fallback shape as `myTurn()`.

### Gating point — **implemented**

`TdmaRadioService::update()` now calls a new `updateRxPower()` instead of calling
`checkIncomingTimeSync()` directly:

```
wantRx = (reliabilityMode != AppLayerAckSummary) || baseRxWindowOpen()
if wantRx:
    if was asleep: log wake, clear asleep flag
    checkIncomingTimeSync()   // available()/receive() re-arm Rx on their own
else:
    if not already asleep: _driver.sleep(); set asleep flag
```

`reliabilityMode != AppLayerAckSummary` covers the `StrictLinkAck` carve-out in one
expression — `baseRxWindowOpen()` is never even evaluated in that mode, so gating is strictly
opt-in for the production reliability mode.

Added `bool sleep()` to `ITdmaRadioDriver`, implemented in `RadioHeadTdmaDriver` as a thin
wrapper over `RH_RF95::sleep()` (already part of the RadioHead dependency in use). No
`FakeRadio` exists yet — `RadioHeadTdmaDriver` is the only concrete implementation, and no
native test currently exercises `TdmaRadioService` (see Testing section), so nothing needed
one. A `FakeRadio` is still required before `TdmaRadioService`-level gating tests (asserting
`sleep()` is called at the right times) can be written.

### Stale-sync fallback

Mirror the existing TX-side fallback: if `_tdmaClock.syncStale()` is true (no `TIME_SYNC` for
`syncStaleMs` = 22 min), `myTurn()` already returns true unconditionally so the node keeps
transmitting. Rx gating should fall back the same way — `baseRxWindowOpen()` returns true
unconditionally while stale, i.e. revert to continuous Rx until a sync is heard again. This
reuses the exact reasoning already documented for the TX side in `tdma-protocol`: don't go
silent (or, here, deaf) indefinitely just because sync lapsed.

### Boundary with goal 1 (pre-sync)

No new logic required beyond what's already there: `hasSync()` is already checked elsewhere
(`drainTxQueue()`'s `hasFreshSync`), so the same predicate gates Rx behavior. Before first
sync, fall into the same "always call checkIncomingTimeSync()" branch as the
`StrictLinkAck`/stale-sync cases above — three different reasons, same safe behavior.

## Implementation checklist

1. **Done.** `ITdmaRadioDriver`: added `virtual bool sleep() = 0;`
   (`include/radio/ITdmaRadioDriver.h`).
2. **Done.** `RadioHeadTdmaDriver::sleep()`: calls `_rf95.sleep()`, guarded by `_healthy`
   (`src/platform/RadioHeadTdmaDriver.cpp`). No new state — RadioHead re-arms the mode
   register on the next `send()`/`sendToWait()`/`available()` call regardless of current mode.
3. **Done.** `TdmaClock::baseRxWindowOpen()` added (`include/radio/TdmaClock.h`,
   `src/radio/TdmaClock.cpp`) — pure function of existing state, no new stored state.
4. **Done.** `TdmaRadioService::update()` now calls a new `updateRxPower()` private method
   (`include/radio/TdmaRadioService.h`, `src/radio/TdmaRadioService.cpp`) which gates
   `checkIncomingTimeSync()` vs. `_driver.sleep()` per the pseudocode above. Tracks
   `_radioAsleep` so `sleep()` isn't called redundantly every tick, and logs `rx_sleep`/
   `rx_wake` transitions via the existing `LOG_DEBUG("radio", ...)` convention.
5. **Done — no new constants needed.** Confirmed `guardMs` alone provides sufficient margin
   without a dedicated wake-ahead constant (see Design section above).
6. **Deferred.** `FakeRadio` (a `test/support/fakes/FakeRadio.h` implementing
   `ITdmaRadioDriver`) does not exist yet. Not created in this pass because no native test
   currently exercises `TdmaRadioService` at all — the native `build_src_filter` only ever
   compiled `power/` and `sensors/` (see item 7's filter note). Needed before
   `TdmaRadioService`-level gating behavior (as opposed to `TdmaClock`'s pure logic) can be
   asserted in a native test.
7. **Partially done.** Added a native test suite for `TdmaClock::baseRxWindowOpen()`
   (`test/test_tdma_clock/test_main.cpp`) covering pre-sync, in/out of slot 0, the slot-0
   wraparound, independence from `mySlot()`/`nodeId`, and the stale-sync fallback. This
   required adding `+<radio/TdmaClock.cpp>` to `[env:native]`'s `build_src_filter`
   (`platformio.ini`) — the first `radio/` file ever compiled into the native test build.
   `TdmaClock.cpp` has no Arduino/RadioHead dependency, so this was a clean, narrow addition;
   `TdmaRadioService.cpp` and `SmartFiresBaseApp.cpp` both depend on `ITdmaRadioDriver` and
   would need `FakeRadio` (item 6) plus a wider filter change — **not done in this pass.**
   The base-side regression test for goal 2 (asserting `SmartFiresBaseApp` never sends outside
   `_baseTdmaClock.myTurn()`) still needs to be written and depends on that wider filter
   change too, since it requires compiling `app/SmartFiresBaseApp.cpp` natively.

## Testing & verification plan

- **Native unit tests** (no hardware): `TdmaClock::baseRxWindowOpen()` is covered (see item 7
  above). Still needed: `TdmaRadioService::updateRxPower()` behavior (requires `FakeRadio`,
  item 6) and the `SmartFiresBaseApp` slot-0-only-send regression test (requires compiling
  `app/` natively, item 7).
- **Bench measurement before/after**: this plan is built on SX1276 datasheet typical currents
  (Rx-continuous ≈ 10.3 mA, sleep ≈ 0.2 µA), not bench-measured values for the actual RFM95
  boards in use. Measure actual current draw (multimeter or INA219 in series with the
  Feather's battery/3V3 rail) in three states — sleep, idle, Rx-continuous — before trusting
  the projected savings below, and again after implementation to confirm the gated behavior
  matches the projection.
- **Soak test**: run a real node for several multiples of the 10-minute `TIME_SYNC` interval
  and the 22-minute stale threshold, confirming `TIME_SYNC`, `ACK_SUMMARY`, and a manually
  triggered `CMD_CALIBRATE` are all still reliably received with Rx gating active, and that
  the stale-sync fallback correctly reopens continuous Rx if a sync is deliberately withheld.
- **Multi-node test**: with `NUM_SLOTS=4` (3 real nodes + base), confirm no node ever needs to
  receive anything during another node's slot — this should be a non-event by construction
  (goal 2), but worth confirming nothing relies on overhearing sibling nodes' traffic anywhere
  in the current codebase before relying on it being safe to sleep through those slots.

## Expected impact (subject to bench verification)

At `NUM_SLOTS=4` (frame = 3,600 ms, slot = 900 ms), using SX1276 datasheet typical currents:

| Scenario | Rx-on time/frame | Avg radio current (approx.) |
|---|---|---|
| Today (Rx-continuous except while TXing) | ~3,260 ms | ~12.1 mA |
| Gated (Rx only during base's 900 ms slot 0) | 900 ms | ~5.3 mA |

That's roughly a **56% cut in average radio current** from gating alone, with no change to
reliability semantics. A later phase could shrink the 900 ms listen window down to just the
guard band plus the base's actual transmit time within slot 0 (likely well under 200 ms),
pushing this toward a ~75% cut — but that requires first measuring how tightly the base's
send timing is bounded within its slot (it can dequeue up to 3 packets per `update()` call,
in priority order, not a fixed single timestamp), so it's listed as future work, not part of
this plan.

This is radio-current only. Per `duty-cycling`, sensor/MCU duty cycling is currently disabled
(`kThresholdEnabled = false`), so sensors run back-to-back continuously regardless of this
change — worth a whole-node current budget pass to see how much of total battery draw this
radio optimization actually moves before investing further here.

## Open questions

- Does the SX1276 need wake-ahead margin beyond the existing 20 ms `guardMs` for oscillator
  startup from sleep, specifically on the RFM95W modules in use? Datasheet says sub-ms for
  the synthesizer but crystal startup can be longer — needs a bench measurement, not a guess.
- Should `baseRxWindowOpen()` live on `TdmaClock` (parallel to `myTurn()`) or on
  `TdmaRadioService` directly? Leaning `TdmaClock` for symmetry with `myTurn()` and because
  it's pure session-clock math with no radio-driver dependency, consistent with `TdmaClock`'s
  existing scope.
- Out of scope for this plan but worth flagging: a future phase-2 could narrow the slot-0
  listen window itself (see "Expected impact" above) once base-side send-timing jitter is
  characterized.
