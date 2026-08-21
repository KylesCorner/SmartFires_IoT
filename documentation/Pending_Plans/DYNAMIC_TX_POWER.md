---
name: dynamic-tx-power
description: Plan for base-station-driven, per-node dynamic TX power adjustment using RSSI and retry telemetry the system already collects.
category: plan-pending
status: in-progress — implemented, flashed and baked. Constants still untuned; base ceiling raise not done.
related_docs:
  - lora-vs-lorawan
  - tdma-protocol
  - packet-reliability
  - tunable-parameters
  - reset-reason-diagnostics
---

# Dynamic TX Power

## Status (audited 2026-08-21)

The control loop is **built, flashed and running**: `TxPowerController` on the base (23 native
tests), `PKT_CMD_SET_TX_POWER` (0x15), `StatusPayload.tx_power_dbm` +
`STATUS_TX_POWER_STATIC`, and per-node DYNAMIC/STATIC controls on the dashboard
(`POST /api/tx_power`). "implemented-unflashed" is no longer accurate.

This stays in `Pending_Plans/` for one reason: **every threshold in `BaseConfig.h`'s
dynamic-TX-power block is a starting value, not a bench-characterised one.** The mechanism
is done; the tuning is the remaining work. See "Still open" and "Open questions" below —
the debug-env question (`SMARTFIRES_STATUS_INTERVAL_MS=1000` against thresholds meant for
15-minute deltas) is the one most likely to bite first.

Also still outstanding and independent of tuning: the base's own TX power ceiling raise to
+20 dBm (a separate static change, documented at the end of this doc), and excluding
BOD-cycling nodes from optimisation.

Note the interaction recorded under "Still open" has not gone away: this feature pays close
to nothing until the WDT reboot rate drops, because every reboot discards a node's converged
level. That makes `reset-reason-diagnostics`' remaining hardware validation a soft
prerequisite for tuning these constants against real data.

## Background

Every node transmits at a single fixed power today — `NetworkConfig::kRadioTxPowerDbm =
13 dBm` (`platformio/include/config/NetworkConfig.h`), applied once in
`RadioHeadTdmaDriver::begin()` and never touched again. A node sitting close to the base
burns exactly as much TX current as one at the edge of range, with no feedback loop in
either direction. See [lora-vs-lorawan](../Current_Architecture/LORA_VS_LORAWAN.md) for
the broader optimization context this plan was carved out of.

The system already collects the two signals this needs, unused for this purpose:

- **Per-frame RSSI at the base.** The base→Jetson UART frame already carries an `rssi`
  byte for every received LoRa payload (`[0xAA][0x55][len][rssi][payload][crc8]` — see
  `UART_JETSON_BRIDGE.md`). The base computes this on every packet it receives from every
  node — it doesn't need to ask for it.
- **Lifetime retry/failure counters in `STATUS`.** Every `PKT_STATUS` (every 15 min)
  carries `retx_total`/`fail_total` (`StatusPayload`, `BinaryPacket.h`), fed by
  `TdmaRadioService::retransmitCount()`/`failedSendCount()`. The Jetson today computes
  per-interval deltas by differencing consecutive `STATUS` packets — that delta math can
  just as easily run on the base, which already owns the per-node assignment table these
  counters would key off of.

Both signals already arrive at the base before they ever reach the Jetson. That's the
basis for the design decision below.

## Decision: control loop lives on the base station, not the Jetson

This is a deliberate choice, not a default. The base already owns:

- `findOrCreateNodeAssignment()` and the per-node assignment table (node_id, slot).
- The per-node `ACK_SUMMARY` bitmap state.
- The freshest possible RSSI reading (it's the radio that received the packet).

Putting the decision loop on the Jetson would mean relaying RSSI/retry data over USB just
to relay a power-change decision right back over USB to the base to forward over LoRa —
two extra hops of latency and another place state can desync (e.g., if the Jetson session
restarts mid-decision). Keeping it on the base means the feedback loop is: receive
packet → update local per-node stats → decide → send, entirely within one Feather, with
no dependency on the Jetson being alive, connected, or in any particular session state.
The Jetson can still read the *current* per-node TX power back (e.g. surfaced in the
existing UART telemetry or a status field) for dashboard visibility, but it does not
decide.

## Goals

1. Reduce average node TX current without compromising link reliability.
2. Keep the decision loop entirely on the base station Feather — no Jetson involvement in
   the control loop itself.
3. Reuse the existing `CMD_*`/`CMD_ACK` round-trip pattern (`CMD_CALIBRATE`/`CMD_RESET`)
   rather than inventing new transport plumbing.
4. Fail safe: a node that loses contact while in a reduced-power state must not get stuck
   there.

## Design

### New packet: `CMD_SET_TX_POWER`

Modeled directly on `CMD_CALIBRATE`/`CMD_RESET` (`PktHeader` + small payload + CRC8),
base→node, acknowledged by the existing `CMD_ACK` (`0x13`) path — no new ack packet type
needed.

**Transport: reuse the existing pending-command queue.** Base-originated
`CMD_SET_TX_POWER` sends go through the same `enqueuePendingCommand()`/`_pendingCommands`
machinery that already defers Jetson-forwarded `CMD_CALIBRATE`/`CMD_RESET` to the base's
slot-0 window — no parallel send path. Two consequences the decision loop must respect:

- `kMaxPendingCommands` is `kMaxAssignedNodes + 1` = `kNumSlots` (5 at NUM_SLOTS=5), *not*
  the literal 4 this plan originally assumed — it was re-derived from the slot count when
  the Jetson's "New Session" flow needed room for a whole-network reset sweep. The queue is
  still *shared* with user-issued calibrate/reset commands, and a full queue silently fails
  to enqueue (`QUEUE_FULL` — the same limitation documented for the per-node Reset button in
  `RESET_SYSTEM.md`). Four nodes all stepping in one STATUS interval *plus* a reset sweep
  still overruns it.
- A failed enqueue must be treated as "no change issued": do **not** set
  `pendingTxPowerDbm` — the loop simply retries the decision at the next STATUS interval.
  Whether `kMaxPendingCommands` needs bumping is a deployment-size question, not one this
  plan resolves.

```
CmdSetTxPowerPayload (as built — BinaryPacket.h, PKT_CMD_SET_TX_POWER = 0x15):
  node_id      : uint8_t  (redundant with PktHeader.node_id, mirrors CMD_RESET's pattern)
  tx_power_dbm : int8_t   (target power, signed to allow future negative-range radios)
```

**The proposed third `seq` byte was dropped.** This plan originally called it "mirrors
existing CMD_* sequencing", but that was a misreading of the existing code: neither
`CmdCalibratePayload` nor `CmdResetPayload` carries a seq — `PktHeader::seq` already
sequences all of them. A second copy could only ever disagree with the header, so the
payload is 2 bytes and the frame is 8, matching CALIBRATE/RESET exactly.

Node-side handling mirrors `CalibrationDebug.cpp`'s dispatch for `CMD_CALIBRATE`/
`CMD_RESET`: decode, apply (`ITdmaRadioDriver` gains a `setTxPower(int8_t)` passthrough to
`RH_RF95::setTxPower()`), log, and reply with `CMD_ACK`.

### Per-node state on the base

Extend the existing per-node assignment record (wherever `findOrCreateNodeAssignment()`
keeps node_id/slot today) with:

```
currentTxPowerDbm   : int8_t   (last commanded value, defaults to kRadioTxPowerDbm)
pendingTxPowerDbm   : optional<int8_t>  (awaiting CMD_ACK confirmation)
rssiHistory         : small rolling window (e.g. last N received-frame RSSI values)
lastRetxTotal/lastFailTotal : uint16_t  (previous STATUS values, for delta computation)
```

This is the same shape of state the base already keeps for `ACK_SUMMARY` tracking
(`kMaxAckTrackedNodes`) — extend that table rather than building a parallel one.

### Decision loop

Runs on the base, gated on `STATUS` packet arrival (15-minute cadence —
`kStatusIntervalMs`) to match the rate at which `retx_total`/`fail_total` deltas become
meaningful, and otherwise updates only the RSSI rolling window on every inbound packet:

```
on STATUS packet from node N:
    retxDelta = retx_total - lastRetxTotal[N]
    failDelta = fail_total - lastFailTotal[N]
    rssiAvg   = average(rssiHistory[N])
    update lastRetxTotal[N], lastFailTotal[N]

    if pendingTxPowerDbm[N] is set:
        // waiting on CMD_ACK from a prior change — don't issue another change yet
        return

    if retxDelta == 0 and failDelta == 0 and rssiAvg > kRssiHeadroomThreshold:
        // comfortably linked for a full STATUS interval — step power down
        target = max(currentTxPowerDbm[N] - kPowerStepDbm, kMinTxPowerDbm)
    elif rssiAvg < kRssiFloorThreshold:
        // uplink itself is weak — step power up immediately, don't wait for it to fail outright
        target = min(currentTxPowerDbm[N] + kPowerStepDbm, kRadioTxPowerDbm /* ceiling = today's static default */)
    elif retxDelta > kRetryAlarmThreshold:
        // retries are high but RSSI is fine — the downlink (ACK_SUMMARY delivery), not the
        // node's TX strength, is the likely fault. Log it; a TX power bump can't fix a
        // downlink problem and would just spend battery for nothing.
        return
    else:
        return  // no change

    if target != currentTxPowerDbm[N]:
        send CMD_SET_TX_POWER(N, target)
        pendingTxPowerDbm[N] = target
```

Key properties:

- **Power-down is conservative (one step per 15-min interval); power-up is responsive**
  (triggered on the first sign of trouble, not after sustained failure) — asymmetric on
  purpose, since the cost of staying too low is lost telemetry, while the cost of staying
  too high is just battery.
- **Ceiling is today's static default (`kRadioTxPowerDbm`), not the radio's hardware max.**
  This plan only ever reduces power below the known-working baseline and walks back
  toward it — it does not explore above what's already deployed and field-validated.
  Raising the baseline ceiling itself is a separate, manual change (see
  [lora-vs-lorawan](../Current_Architecture/LORA_VS_LORAWAN.md)'s range section), not
  something this control loop should do on its own.
- **One change in flight per node at a time** (`pendingTxPowerDbm` gate) — avoids
  compounding an unconfirmed change with another before knowing if the node received it.
- **RSSI and retry-rate are different fault signals, not interchangeable triggers.** RSSI
  reflects uplink signal strength only (measured at the base on receipt). `retx_total`
  growth under `AppLayerAckSummary` mode is driven by the node not seeing its packet
  confirmed in `ACK_SUMMARY` — which depends on the downlink. High retry rate with healthy
  RSSI usually means the downlink, not the node's TX strength, is the fault — raising the
  node's TX power doesn't fix that. Only a low `rssiAvg` triggers a power-up; a high
  `retxDelta` alone is logged for visibility, not acted on by this loop.

### Fail-safe behavior

- **Boot default is always the static baseline.** A node that reboots (power cycle, MCU
  reset, WDT recovery, hard `CMD_RESET`) comes back up at `kRadioTxPowerDbm`, not whatever
  the base last commanded. The base-side state reset does **not** fall out of
  `findOrCreateNodeAssignment()` for free — for a known `uid_hash` that function *reuses*
  the existing assignment record, so extended fields would persist across the node's
  reboot. The base's AWAKEN handler already explicitly calls
  `resetAckTracker(assignment->nodeId, "awaken")` for exactly this reason
  (`SmartFiresBaseApp.cpp`, AWAKEN dispatch); reset
  `currentTxPowerDbm`/`pendingTxPowerDbm`/`rssiHistory`/`lastRetxTotal`/`lastFailTotal` at
  that same call site. One explicit line, same shape as the existing tracker reset. Bonus:
  the AWAKEN payload now carries `reset_cause`/`hang_zone` (see
  `RESET_REASON_DIAGNOSTICS.md`), so the base's debug log line for this reset can be
  bucketed by why the node rebooted.
- **Unconfirmed change reverts on timeout.** If `CMD_ACK` doesn't arrive within a bounded
  window, clear `pendingTxPowerDbm` and re-arm for another decision next `STATUS` interval
  rather than assuming the change took effect. Note the existing `CLI_CMD_ACK_TIMEOUT_S`
  (5 s, `edge-receiver`'s `config.py`) is a *Jetson-side* warn timeout — the base has no
  equivalent today, so this needs a new base-side constant (`kCmdAckTimeoutMs`,
  `BaseConfig.h`) rather than a mirror of existing base code.
- **Stale-sync-style safety net.** If a node goes quiet for longer than
  `kSyncStaleMs`-equivalent at the base's per-node tracking layer, treat its
  `currentTxPowerDbm` as unknown and don't issue further deltas relative to it until it
  re-establishes contact (re-`AWAKEN`s), to avoid stacking blind adjustments onto a node
  that might already be back at its hardware-reset default.
- **The STATUS-gated trigger above is blind to a fully-dark uplink — pair it with a
  silence timeout.** The decision loop only runs when a STATUS packet arrives; a node
  whose uplink dies completely never sends one, so the loop never fires and never asks it
  to raise power. Track time-since-last-received-packet-of-any-kind per node (already free
  — it's the same timestamp the `rssiHistory` update touches on every inbound frame) and
  once it exceeds roughly `2 × kStatusIntervalMs` (the same 2×-interval shape as
  `syncStaleMs`), treat the node as link-down by the rule above, and optionally send a
  small, bounded number of best-effort `CMD_SET_TX_POWER(N, kRadioTxPowerDbm)` probes in
  case only the uplink — not the downlink — is actually dead.
- **Node-side mirror of the same fallback.** `TdmaClock` already reverts to immediate-TX
  behavior once it hasn't seen a TIME_SYNC within `syncStaleMs` (22 min). Extend that same
  trigger to also reset the node's local `currentTxPowerDbm` to `kRadioTxPowerDbm`. This
  doesn't reintroduce a second decision-maker (see "Decision" above) — the node isn't
  judging link quality, it's discarding a base instruction it can no longer trust and
  falling back to the one value already known to work. No new packet or bandwidth needed;
  it reuses clock state the firmware already maintains.

### Interaction with the watchdog reboot rate

The hardware WDT is now shipped on both node and base, and the overnight 2026-07-14 soak
recorded **15 WDT-recovered node reboots in one night**. Under the boot-default fail-safe
above, every one of those reboots discards the node's converged power level and restarts
it at the full `kRadioTxPowerDbm` baseline — and reconvergence is deliberately slow (one
`kPowerStepDbm` step per 15-minute STATUS interval, so e.g. 13 → 5 dBm takes a full hour
of clean link). At the observed reboot rate, a node could spend most of its time walking
back down rather than sitting at its converged level, eroding most of the battery savings
this plan exists to capture.

This is a sequencing dependency, not a design flaw: the fail-safe is correct (a
just-rebooted node at reduced power with no base contact is exactly the stuck state the
plan must avoid), and `RESET_REASON_DIAGNOSTICS.md` (phases 1–2 implemented) exists to
attribute and fix those hangs — current attribution points at the RadioHead
`waitPacketSent()` path and shared-I2C stalls. **Land the hang fixes, or at least confirm
the reboot rate has dropped to a few per week, before expecting this plan's projected
savings to materialize.** The control loop itself can ship earlier — it's correct at any
reboot rate, just less profitable at a high one — and its per-node debug log of
AWAKEN-triggered power resets (bucketed by `reset_cause`, see above) doubles as a free
measurement of exactly how much convergence time reboots are costing.

### Link liveness — what "active" means, and why TIME_SYNC frequency isn't the lever

"Link active" is defined separately for each direction, from state each side already
maintains — no new packet type:

- **Base's view of a node:** time since the last received packet of any kind from that
  node (already touched on every inbound frame for the `rssiHistory` update above).
- **Node's view of the base:** `TdmaClock::hasSync()` / time since the last applied
  TIME_SYNC, already tracked for the existing `syncStaleMs` fallback.

Sending TIME_SYNC more frequently doesn't sharpen either of these. It's broadcast and
fire-and-forget, so a node hearing it more often confirms only that the node's *receiver*
still works — it says nothing about whether that node's own uplink is reaching the base,
which is the actual blind spot identified above. The fix for that blind spot is the
base-side silence timeout, not a faster TIME_SYNC cadence.

### Telemetry and observability

The base already tracks `currentTxPowerDbm` per node for its own decision loop (see
above), but that's base-local state — it doesn't reach the Jetson or get logged anywhere a
person can see it without separately surfacing it. Close that loop the same way
heading/battery already are: put the value on the wire.

- **`StatusPayload` gains a `tx_power_dbm` field** — the node's own record of its
  currently-applied TX power, written at STATUS-encode time. Sourcing it from the node
  (what it actually applied) rather than the base (what it last commanded) means the
  Jetson sees ground truth even if a `CMD_SET_TX_POWER`/`CMD_ACK` round-trip is mid-flight
  or was missed — same reasoning as the fail-safe section's insistence on treating
  unconfirmed state as unknown.
- **Node-side serial log** on every applied `CMD_SET_TX_POWER` (old value, new value, seq),
  next to the existing `CMD_CALIBRATE`/`CMD_RESET` log lines in `CalibrationDebug.cpp` —
  this is what a bench/field engineer watching the node's serial monitor needs to see to
  confirm a step actually landed.
- **Base-side debug log** at both ends of the decision: when the loop *decides* to send
  `CMD_SET_TX_POWER` (node, target, and *why* — RSSI floor, headroom step-down, or a
  silence-timeout probe) and when the `CMD_ACK` confirms or times out. The base already has
  a path for this straight to the web dashboard — `DebugLogger` → `FramedDebugLogSink` →
  `PKT_DEBUG_LOG` → the Jetson's `/debug` page and `/ws/base-debug` stream — so this is new
  log call sites, not new plumbing.
- **Web node list** shows per-node TX power as a column, sourced from
  `StatusPayload.tx_power_dbm` (not the base's own `currentTxPowerDbm`, for the same
  ground-truth reason above).

This gives a power change three separate, traceable touchpoints instead of one
base-internal variable: node applies it and logs it locally → the new value rides home in
the next STATUS → the base separately logs its own decision/ack timeline → the web surfaces
both the current value (node list) and the change history (base debug log stream).

## Implementation status

**Implemented 2026-08-19, unflashed. Threshold constants are untuned starting values.**
The full loop is in place — transport, telemetry, the base-side decision loop, both
fail-safes, and per-node operator control from the dashboard. What has *not* happened is
any bench or field characterisation, so every constant in `BaseConfig.h`'s dynamic-TX-power
block should be treated as a guess with a rationale, not a tuned value.

### The organising principle the design settled on

Every failure mode is owned by **whichever side still has a working link**. This is what
resolves the "should a node raise its own power when acks stop?" question, and it is worth
stating before the mechanics:

| Failure | Who can still act | Owner |
|---|---|---|
| Downlink dead (node hears nothing) | Node only | Node self-reverts to baseline on stale sync |
| Uplink dead (base hears nothing, node still hears) | Base only | Base probes node back to baseline |
| Both dead | Neither | Node self-reverts; nothing else matters |
| Neither dead | Both | Base's control loop decides |

No case has two owners and no case has zero. The node's entire share of authority is
*discarding an instruction it can no longer trust* — never judging link quality. Because
the baseline is the ceiling in this design, that revert is always a step up or a no-op:
monotonic, terminal, and incapable of oscillating or fighting the base.

**Missed `ACK_SUMMARY` is deliberately not a node-side trigger.** It is ambiguous in at
least five ways — weak uplink, weak downlink, Rx gating missing the slot-0 window, normal
Timed-standby ack deferral, and the base's own `kMaxAckSummarySendAttempts` circuit breaker
— and only the first is fixable by raising node TX power. Stale sync has none of that
ambiguity and strictly dominates it as a signal: `ACK_SUMMARY` and `TIME_SYNC` ride the same
downlink, from the same transmitter, in the same slot-0 window, so any downlink failure bad
enough to eat acks eats syncs too — but `TIME_SYNC` goes out unconditionally every
`kPeriodicTimeSyncMs` whether or not there is anything to say, while `ACK_SUMMARY` is
conditional, rate-limited and deferred. One trigger, the one that already existed.

### Deviations from this plan as originally drafted

1. **SNR, not RSSI.** RSSI cannot express LoRa link margin — the modem demodulates below the
   noise floor (≈ -7.5 dB SNR at SF7), so two frames at equal RSSI can be comfortable or one
   fade from dropping. `ITdmaRadioDriver::ReceivedPacket` gained an `snr` field
   (`RH_RF95::lastSNR()`, already whole dB).
2. **One margin target plus a dead band, not two thresholds.** The drafted
   `kRssiFloorThreshold`/`kRssiHeadroomThreshold` pair can be set into an oscillation (floor
   above headroom) with nothing to catch it. `kTargetSnrMarginDbX10` + `kSnrDeadBandDbX10`
   cannot, and the relationship is enforced by `static_assert`.
3. **Power-up is a single jump to baseline, not a step.** The cost asymmetry is large enough
   that feeling the way back up is indefensible: too high costs a sliver of battery, too low
   costs telemetry and the retries to recover it. Also makes base-side recovery and the
   node-side fallback the *same* action, so there is one recovery state reached two ways.
4. **`retx`/`fail` inhibit a step-down; they never trigger a step-up.** The drafted
   `kRetryAlarmThreshold` would have to be calibrated against a duty-cycle-structural floor
   (the base defers acks across standby by design, and a lost window marker costs a
   retransmission by design) that moves whenever the window period changes. Retries are
   trustworthy in exactly one direction — evidence the node is not comfortable, no evidence
   whose fault it is — so they gate the optimisation and nothing more.
5. **The payload's proposed third `seq` byte was dropped**; `PktHeader::seq` already
   sequences CMD_* and the existing CALIBRATE/RESET payloads carry none.
6. **The debug-env open question is resolved by construction, not by a policy choice.**
   Pacing decisions on `kTxPowerMinDecisionIntervalMs` — the controller's own clock — rather
   than on STATUS arrival makes behaviour identical whether STATUS comes every second or
   every 15 minutes, and makes the retx/fail delta window match the decision window. None of
   the three options this plan originally offered (disable below a floor / let it run fast /
   scale thresholds) was needed.
7. **Added: per-node DYNAMIC/STATIC mode**, carried in the command payload and reported back
   in `StatusPayload.flags` (`STATUS_TX_POWER_STATIC`, free — the byte had a spare bit).
   STATIC is an operator override for bench and range work, not a safety state: a node that
   loses contact reverts to DYNAMIC at baseline along with everything else, because keeping
   *one* fallback rule matters more than preserving an experiment's fidelity.

### Things found while building that the checklist did not anticipate

- **`TdmaRadioService`'s command RX allowlist** admitted only `PKT_CMD_CALIBRATE`/
  `PKT_CMD_RESET`. Without adding the new type there the node ACKs nothing and the frame is
  logged as `rx_unhandled`. Any future command type hits the same trap.
- **`csv_logger`'s `DictWriter` has no `extrasaction='ignore'`**, so a new key in a status
  row raises unless the column is added to `CSV_COLUMNS`.
- **`kMaxPendingCommands` is no longer the literal 4** this plan assumed — it is
  `kMaxAssignedNodes + 1` = `kNumSlots`.
- **`kCmdAckTimeoutMs` must exceed the Timed duty-cycle period** (75 s), not a frame period.
  A command queued while a node is in standby cannot be delivered until its next
  `WINDOW_BEGIN`; sized off a frame period the base would time out and re-arm on every node
  that was merely asleep.
- **Brownout inverts the fail-safe, and is deliberately left out of this loop.** A BOD reset
  already returns the node to baseline for free (nothing is persisted), but that is the
  maximum-current state, and post-reset the node broadcasts `AWAKEN` every 5 s at full power
  — a positive feedback term on an already-sagging battery. The energy at stake in a few
  hundred ms of airtime is small next to duty cycling and battery health, so this belongs in
  a battery-gated startup policy, not folded into a link-quality control loop. What the loop
  *should* eventually do is narrow: the AWAKEN payload already carries `reset_cause`, so the
  base can bucket a BOD-cycling node and stop optimising it, since a node rebooting every few
  minutes never converges and a too-low setting costs more in retries than it saves. **Not
  implemented — see Open questions.**

### Still open

- Every constant in `BaseConfig.h`'s dynamic-TX-power block needs bench/field characterisation.
- The base's own TX power ceiling raise to +20 dBm (step 13) — a separate static change.
- BOD-cycling nodes are not yet excluded from optimisation (see above).
- **This feature pays roughly nothing until the WDT reboot rate drops.** At the 2026-07-14
  rate of 15 reboots/night (~1 per 45 min) and one step per 60 s decision interval, a node
  reaches its converged level only if the link is clean for several minutes at a time — but
  every reboot discards it. The loop is *correct* at any reboot rate, just unprofitable at a
  high one. See `RESET_REASON_DIAGNOSTICS.md`.

## Implementation checklist

1. **[done]** Add `CMD_SET_TX_POWER` packet type + payload to `BinaryPacket.h` and `packet.py`
   (passive decode only on the Jetson side — it doesn't originate or act on these).
2. **[done]** Add `tx_power_dbm` (`int8_t`) to `StatusPayload` (`BinaryPacket.h`), set to the node's
   currently-applied TX power at STATUS-encode time; mirror the field in `packet.py`. Bumps
   `STATUS` from 20 to 21 payload bytes — **26 to 27 bytes on air**, not the "25 to 26" this
   plan said before: `PktHeader` has been 5 bytes since the flags byte landed, so STATUS was
   already 26. Update `CLAUDE.md`'s wire-protocol tables and `BANDWIDTH_SCALING.md`
   accordingly.
3. **[done]** Add `ITdmaRadioDriver::setTxPower(int8_t)` and the `RadioHeadTdmaDriver` implementation
   (thin wrapper over `RH_RF95::setTxPower()`, same pattern as the existing `sleep()`
   passthrough from `radio-rx-gating`).
4. **[done]** Add node-side dispatch in `CalibrationDebug.cpp`/`SmartFiresNodeApp.cpp` alongside the
   existing `CMD_CALIBRATE`/`CMD_RESET` handling, including a serial (`@SFDBG`) log line on
   apply — old value, new value, seq — matching the existing CALIBRATE/RESET log pattern.
5. **[done]** Extend the base's per-node assignment table with the state fields above, and reset
   those fields in the AWAKEN handler alongside the existing
   `resetAckTracker(nodeId, "awaken")` call (see "Fail-safe behavior" — this does *not*
   happen automatically via `findOrCreateNodeAssignment()`).
6. **[done]** Implement the decision loop in `SmartFiresBaseApp` (or a small new
   `TxPowerController`-style collaborator it owns), gated on `STATUS` arrival the same way
   `ACK_SUMMARY` dispatch is gated on TDMA slot 0. Sends go through the existing
   `enqueuePendingCommand()` queue; a failed (queue-full) enqueue leaves
   `pendingTxPowerDbm` unset so the decision re-arms next interval.
7. **[done]** Add base-side `DebugLogger`/`FramedDebugLogSink` log lines (same `PKT_DEBUG_LOG` path
   already streamed to the Jetson's `/debug` page and `/ws/base-debug` stream) at both
   decision points: when the loop issues a `CMD_SET_TX_POWER` (node, target, triggering
   reason — RSSI floor, headroom step-down, or silence-timeout probe) and when the
   corresponding `CMD_ACK` confirms or times out.
8. **[done, renamed]** Add `kPowerStepDbm`, `kMinTxPowerDbm`, `kRssiHeadroomThreshold`,
   `kRssiFloorThreshold`, `kRetryAlarmThreshold`, and `kCmdAckTimeoutMs` (base-side
   CMD_ACK revert window — no base-side equivalent exists today) to `BaseConfig.h`,
   documented in `tunable-parameters`.
9. **[done]** Native tests using `FakeRadio`/`FakeClock` (per the gap already flagged in
   `radio-rx-gating`'s testing section) — this plan adds another consumer that needs that
   fake, worth building it once for both.
10. **[done]** `tx_power_dbm` reaches the CSV/JSONL/ingest log, and the map-history
    node table shows per-node TX power + mode with set/±/pin controls (`/api/tx_power`).
    Original text: surface per-node TX power as a column in the Jetson web dashboard's node list
    (read-only, visibility only, not control) — sourced from the new
    `StatusPayload.tx_power_dbm` field (item 2) so the displayed value reflects what the
    node has actually confirmed applying, not just what the base last commanded.
11. **[done]** Add the base-side per-node silence timeout (≈`2 × kStatusIntervalMs`) alongside the
    STATUS-gated trigger, including the bounded best-effort `CMD_SET_TX_POWER` probe
    behavior described in "Fail-safe behavior" above.
12. **[done]** Add the node-side reset of `currentTxPowerDbm` to `kRadioTxPowerDbm` on `TdmaClock`
    stale-sync (reuse `syncStaleMs`, no new packet).
13. **[not started]** Add `kBaseRadioTxPowerDbm` (`BaseConfig.h`) and wire the base's `main.cpp`
    construction to use it instead of inheriting the shared `NetworkConfig::kRadioTxPowerDbm`
    default — see "Base station TX power ceiling" below.

## Open questions

- Exact values for `kPowerStepDbm`/threshold constants need bench/field tuning, not just
  picked analytically — start conservative (e.g. 2 dB steps) and tune from soak-test data.
- Whether `rssiHistory` should be a fixed-size ring buffer sized to one `STATUS` interval's
  worth of packets, or a simple exponential moving average — ring buffer is more
  auditable, EMA is cheaper on the Feather's RAM budget.
- Whether the per-node TX power state should be persisted across a *base* reboot (today
  nothing here would survive one) or simply re-derived from scratch, with every node
  walking back down from the baseline ceiling again — re-deriving is simpler and self-
  correcting, but means a base reboot temporarily costs the battery savings already earned
  until the loop re-converges.
- Exact silence-timeout multiplier (`2 × kStatusIntervalMs` assumed above) and the bound on
  how many best-effort `CMD_SET_TX_POWER` probes to send into silence before giving up on a
  node — both need bench/field tuning, not just picked analytically, same as the power-step
  constants above.
- Whether +20 dBm on the base (see below) needs a per-deployment regulatory check against
  the actual antenna gain in use, or whether it's safely under Part 15 limits for every
  antenna this project is likely to use — not resolved here.
- Behavior under the debug build: `feather_m0_lora_node_debug` sets
  `SMARTFIRES_STATUS_INTERVAL_MS=1000`, so the STATUS-gated loop would fire every second
  with thresholds tuned for 15-minute deltas — likely churning power changes constantly on
  the bench. Options: disable the loop below some minimum interval, scale thresholds by
  the interval, or accept it as intentionally fast convergence for bench testing. Needs a
  decision before flashing the debug env with this feature.

## Base station TX power ceiling (separate, static change)

The per-node control loop above only ever walks a *node's* TX power up toward
`kRadioTxPowerDbm` — it never questions whether the base's own TX power is part of the
problem. It can be: every base→node frame (`TIME_SYNC`, `ACK_SUMMARY`,
`CMD_CALIBRATE`/`CMD_RESET`, and now `CMD_SET_TX_POWER`) is fire-and-forget with no
app-layer retry, so a weak downlink is invisible to this plan's control loop and can
masquerade as a node-side problem (see the RSSI-vs-retry disambiguation note above).

Today the base and every node share one constant — `NetworkConfig::kRadioTxPowerDbm = 13`
(`NetworkConfig.h:103`) — pulled through the same `RadioHeadTdmaDriver::Config::radioHeadCfg()`
factory and applied identically by the `_rf95.setTxPower(_cfg.txPowerDbm, false)` call in
`RadioHeadTdmaDriver::begin()` (`RadioHeadTdmaDriver.cpp`). There's no role split, so the
base can't be raised without also raising every node's static ceiling.

**Decision: split it, and raise the base.** The base runs off the Jetson/USB supply, not a
node battery, so TX current is free there in a way it categorically isn't for nodes — this
plan's entire reason for *not* just running every node at max power doesn't apply to the
base. Add a `kBaseRadioTxPowerDbm` constant (`BaseConfig.h`) and have the base's
`main.cpp` construction (the `radioHeadCfg(0x01)` call under `#if defined(LORA_BASE)`)
use it instead of inheriting the shared default. Leave
`NetworkConfig::kRadioTxPowerDbm` untouched — it stays the per-node ceiling this plan's
control loop targets.

**Target +20 dBm, not the hardware max of +23 dBm.** `setTxPower()` is already called with
`useRFO=false` (PA_BOOST), which RadioHead clamps to +5..+23 dBm. +20 dBm is the top of
that range without engaging the SX1276's high-power `PA_DAC` trim (which RadioHead
auto-enables above +20 dBm) — only 3 dB of headroom is given up for a cleaner margin under
the chip's rated limits. Revisit +23 dBm later only if field soak data shows the extra 3 dB
is actually needed.

**Regulatory caveat.** Verify +20 dBm conducted is within Part 15 limits for the configured
bandwidth/SF and the deployed antenna's gain before shipping this — not re-derived here
since it depends on the specific antenna in use.

## Footnote: dynamic spreading factor is deferred

This plan deliberately covers TX power only. Dynamic per-node **spreading factor** is a
larger change we're choosing not to take on yet: the base station has a single SX1276
demodulator, which can only be configured for one SF/BW combination at a time. With nodes
on different SFs, the base would need to either round-robin its demodulator settings
across the TDMA frame (risky — it could miss a node's slot entirely if it's mid-switch, or
miss an `AWAKEN` from an unsynced node broadcasting on a different SF than whatever the
base happens to be listening on at that instant) or gain genuinely concurrent multi-SF
reception, which means different base hardware — a multi-demodulator LoRa concentrator
(e.g. Semtech SX1301/SX1302-class chips, as used in LoRaWAN gateways) that can decode
several SF channels simultaneously instead of one SX1276 listening to one configuration at
a time.

We'll revisit dynamic SF once/if the base station moves to that kind of multi-demodulator
hardware. Until then, TX power is the only dynamic lever — it's also the dominant battery
lever anyway, per [lora-vs-lorawan](../Current_Architecture/LORA_VS_LORAWAN.md): TX current
draw is roughly constant per millisecond of airtime regardless of SF, so power reduction
saves energy directly while SF reduction would only save airtime, not current per bit.
