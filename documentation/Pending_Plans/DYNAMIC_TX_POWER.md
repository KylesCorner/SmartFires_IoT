---
name: dynamic-tx-power
description: Plan for base-station-driven, per-node dynamic TX power adjustment using RSSI and retry telemetry the system already collects.
category: plan-pending
status: draft
related_docs:
  - lora-vs-lorawan
  - tdma-protocol
  - packet-reliability
  - tunable-parameters
---

# Dynamic TX Power

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

```
CmdSetTxPowerPayload (proposed):
  node_id   : uint8_t   (redundant with PktHeader.node_id, mirrors CMD_RESET's pattern)
  tx_power_dbm : int8_t  (new target power, signed to allow future negative-range radios)
  seq       : uint8_t   (mirrors existing CMD_* sequencing)
```

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
  reset, hard `CMD_RESET`) comes back up at `kRadioTxPowerDbm`, not whatever the base last
  commanded — the base's per-node state is reset to match on `findOrCreateNodeAssignment()`
  re-registration (a fresh `AWAKEN` after reset looks identical to a first-ever boot from
  the base's point of view, so this falls out of existing logic for free).
- **Unconfirmed change reverts on timeout.** If `CMD_ACK` doesn't arrive within a bounded
  window (mirror the existing `CLI_CMD_ACK_TIMEOUT_S`-style pattern used for
  calibrate/reset), clear `pendingTxPowerDbm` and re-arm for another decision next
  `STATUS` interval rather than assuming the change took effect.
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

## Implementation checklist (not started)

1. Add `CMD_SET_TX_POWER` packet type + payload to `BinaryPacket.h` and `packet.py`
   (passive decode only on the Jetson side — it doesn't originate or act on these).
2. Add `tx_power_dbm` (`int8_t`) to `StatusPayload` (`BinaryPacket.h`), set to the node's
   currently-applied TX power at STATUS-encode time; mirror the field in `packet.py`. Bumps
   `STATUS` from 20 to 21 payload bytes (25 to 26 bytes on air) — update `CLAUDE.md`'s
   wire-protocol tables and `BANDWIDTH_SCALING.md` accordingly.
3. Add `ITdmaRadioDriver::setTxPower(int8_t)` and the `RadioHeadTdmaDriver` implementation
   (thin wrapper over `RH_RF95::setTxPower()`, same pattern as the existing `sleep()`
   passthrough from `radio-rx-gating`).
4. Add node-side dispatch in `CalibrationDebug.cpp`/`SmartFiresNodeApp.cpp` alongside the
   existing `CMD_CALIBRATE`/`CMD_RESET` handling, including a serial (`@SFDBG`) log line on
   apply — old value, new value, seq — matching the existing CALIBRATE/RESET log pattern.
5. Extend the base's per-node assignment table with the state fields above.
6. Implement the decision loop in `SmartFiresBaseApp` (or a small new
   `TxPowerController`-style collaborator it owns), gated on `STATUS` arrival the same way
   `ACK_SUMMARY` dispatch is gated on TDMA slot 0.
7. Add base-side `DebugLogger`/`FramedDebugLogSink` log lines (same `PKT_DEBUG_LOG` path
   already streamed to the Jetson's `/debug` page and `/ws/base-debug` stream) at both
   decision points: when the loop issues a `CMD_SET_TX_POWER` (node, target, triggering
   reason — RSSI floor, headroom step-down, or silence-timeout probe) and when the
   corresponding `CMD_ACK` confirms or times out.
8. Add `kPowerStepDbm`, `kMinTxPowerDbm`, `kRssiHeadroomThreshold`,
   `kRssiFloorThreshold`, `kRetryAlarmThreshold` to `BaseConfig.h`, documented in
   `tunable-parameters`.
9. Native tests using `FakeRadio`/`FakeClock` (per the gap already flagged in
   `radio-rx-gating`'s testing section) — this plan adds another consumer that needs that
   fake, worth building it once for both.
10. Surface per-node TX power as a column in the Jetson web dashboard's node list
    (read-only, visibility only, not control) — sourced from the new
    `StatusPayload.tx_power_dbm` field (item 2) so the displayed value reflects what the
    node has actually confirmed applying, not just what the base last commanded.
11. Add the base-side per-node silence timeout (≈`2 × kStatusIntervalMs`) alongside the
    STATUS-gated trigger, including the bounded best-effort `CMD_SET_TX_POWER` probe
    behavior described in "Fail-safe behavior" above.
12. Add the node-side reset of `currentTxPowerDbm` to `kRadioTxPowerDbm` on `TdmaClock`
    stale-sync (reuse `syncStaleMs`, no new packet).
13. Add `kBaseRadioTxPowerDbm` (`BaseConfig.h`) and wire the base's `main.cpp`
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

## Base station TX power ceiling (separate, static change)

The per-node control loop above only ever walks a *node's* TX power up toward
`kRadioTxPowerDbm` — it never questions whether the base's own TX power is part of the
problem. It can be: every base→node frame (`TIME_SYNC`, `ACK_SUMMARY`,
`CMD_CALIBRATE`/`CMD_RESET`, and now `CMD_SET_TX_POWER`) is fire-and-forget with no
app-layer retry, so a weak downlink is invisible to this plan's control loop and can
masquerade as a node-side problem (see the RSSI-vs-retry disambiguation note above).

Today the base and every node share one constant — `NetworkConfig::kRadioTxPowerDbm = 13`
(`NetworkConfig.h:103`) — pulled through the same `RadioHeadTdmaDriver::Config::radioHeadCfg()`
factory and applied identically by `RadioHeadTdmaDriver.cpp:41`'s
`_rf95.setTxPower(_cfg.txPowerDbm, false)`. There's no role split, so the base can't be
raised without also raising every node's static ceiling.

**Decision: split it, and raise the base.** The base runs off the Jetson/USB supply, not a
node battery, so TX current is free there in a way it categorically isn't for nodes — this
plan's entire reason for *not* just running every node at max power doesn't apply to the
base. Add a `kBaseRadioTxPowerDbm` constant (`BaseConfig.h`) and have the base's
`main.cpp` construction (currently lines 17-19, `radioHeadCfg(0x01)` under
`#if defined(LORA_BASE)`) use it instead of inheriting the shared default. Leave
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
