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
    elif retxDelta > kRetryAlarmThreshold or rssiAvg < kRssiFloorThreshold:
        // link degrading — step power up immediately, don't wait for it to fail outright
        target = min(currentTxPowerDbm[N] + kPowerStepDbm, kRadioTxPowerDbm /* ceiling = today's static default */)
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

## Implementation checklist (not started)

1. Add `CMD_SET_TX_POWER` packet type + payload to `BinaryPacket.h` and `packet.py`
   (passive decode only on the Jetson side — it doesn't originate or act on these).
2. Add `ITdmaRadioDriver::setTxPower(int8_t)` and the `RadioHeadTdmaDriver` implementation
   (thin wrapper over `RH_RF95::setTxPower()`, same pattern as the existing `sleep()`
   passthrough from `radio-rx-gating`).
3. Add node-side dispatch in `CalibrationDebug.cpp`/`SmartFiresNodeApp.cpp` alongside the
   existing `CMD_CALIBRATE`/`CMD_RESET` handling.
4. Extend the base's per-node assignment table with the state fields above.
5. Implement the decision loop in `SmartFiresBaseApp` (or a small new
   `TxPowerController`-style collaborator it owns), gated on `STATUS` arrival the same way
   `ACK_SUMMARY` dispatch is gated on TDMA slot 0.
6. Add `kPowerStepDbm`, `kMinTxPowerDbm`, `kRssiHeadroomThreshold`,
   `kRssiFloorThreshold`, `kRetryAlarmThreshold` to `BaseConfig.h`, documented in
   `tunable-parameters`.
7. Native tests using `FakeRadio`/`FakeClock` (per the gap already flagged in
   `radio-rx-gating`'s testing section) — this plan adds another consumer that needs that
   fake, worth building it once for both.
8. Surface `currentTxPowerDbm` per node on the Jetson dashboard (read-only) — visibility
   only, not control.

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
