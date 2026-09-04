---
name: dynamic-tx-power
description: Remaining field-tuning and ceiling decisions for the shipped base-owned per-node TX-power controller.
category: plan-pending
status: draft
related_docs:
  - lora-vs-lorawan
  - tdma-protocol
  - packet-reliability
  - tunable-parameters
  - reset-reason-diagnostics
---

# Dynamic TX power: remaining work

## Status at 2026-09-04

The mechanism is implemented, flashed, and baked into current base/node builds. This file remains pending only because controller constants have not been bench/field tuned and the project's desired upper TX-power ceiling is unresolved.

Shipped pieces:

- Base `TxPowerController` tracks per-node SNR, STATUS link-counter deltas, mode, current/pending power, command timeout, and silence probes.
- `PKT_CMD_SET_TX_POWER` (`0x15`) carries `{node_id:u8, tx_power_dbm:i8, mode:u8}`; the complete LoRa-format frame is 9 bytes.
- The node clamps and applies an absolute value, stores DYNAMIC/STATIC mode, reports both in STATUS, and sends `CMD_ACK`.
- The base collects SNR from every decoded frame belonging to an assigned node. STATUS provides retransmit/failure totals and the node's authoritative applied value/mode.
- The dashboard `/api/tx_power` route resolves DYNAMIC, STATIC, set, increase, and decrease on the server into an absolute request. The ingest loop relays that request; no relative wire command exists.
- Base commands use fire-and-forget LoRa in slot 0. Delivery confirmation is application-layer `CMD_ACK`, normally returned in the node's slot.
- Node and base fail-safe paths restore/probe the baseline when state is stale or contact is lost.

## Shipped decision rule

The base's values are defined in `BaseConfig.h` and summarized in `TUNABLE_PARAMETERS.md`:

- SX1276 SF7 demodulation floor assumption: -7.5 dB SNR.
- Target link margin: 10 dB above that floor.
- Step-down dead band: 3 dB above target.
- Step-down size: 2 dBm.
- Allowed node range: 5–13 dBm.
- Minimum interval between decisions for one node: 60 seconds.

On STATUS, the controller averages SNR samples gathered since the previous decision and differences the saturated 16-bit retry/failure totals.

- If margin is below target, it jumps directly to the 13 dBm baseline.
- If margin is above target plus dead band and neither counter increased, it steps down 2 dBm.
- If retries/failures increased, it inhibits a step-down but does not use that fact alone to step up. App-layer retries can indicate a base-to-node ACK problem rather than weak node uplink.
- STATIC mode bypasses automatic decisions.
- Only one power command may be in flight per node.

The node's next STATUS is ground truth. It can correct a base assumption after a lost `CMD_ACK`, a clamped request, or reboot.

## Shipped recovery rules

- A node boots at 13 dBm DYNAMIC. `AWAKEN` resets the base's controller record for that node.
- A node whose session sync becomes stale restores 13 dBm DYNAMIC locally, because a failed downlink prevents the base from commanding recovery.
- A base that hears nothing for 5 minutes may send an absolute 13 dBm silence probe if it believes the node is below baseline. It makes at most three consecutive probes.
- A pending power command expires after 120 seconds so a sleeping Timed node gets a chance to wake. Expiry does not assume whether the request landed; STATUS reconciles it.
- STATIC is an operator experiment, not persistent safety state. It is discarded across node reboot/stale-session fallback.

## Remaining work

### 1. Characterize and tune controller constants

Run repeatable link tests across representative distance, antenna orientation, obstruction, base placement, node supply voltage, and environmental conditions. Capture:

- per-frame SNR distribution at each commanded dBm;
- BUNDLE/STATUS first-send and retransmission counts;
- ACK-summary delivery behavior and window-marker loss;
- node current/energy per Timed or SensorTriggered cycle;
- time to step down and time to recover to baseline;
- resets/brownouts during low battery or high TX current.

Choose the demod-floor assumption, target margin, dead band, step size, decision interval, command timeout, silence timeout, and probe budget from observed distributions. Timed-node ACK deferral creates retries unrelated to uplink strength, so tune with window markers and base downlink health visible.

### 2. Decide whether 13 dBm remains the ceiling

The controller deliberately cannot exceed the field-used 13 dBm baseline. Raising the ceiling (for example to 20 dBm) is a separate hardware/regulatory decision and would require:

- verifying the RFM95 PA path, supply and thermal/current headroom;
- checking the actual jurisdiction, frequency plan, antenna gain, and allowed radiated power/dwell behavior;
- changing the network baseline/clamp and the Python dashboard constants together;
- measuring slot airtime only if modem parameters also change (power alone does not change airtime);
- repeating brownout/watchdog tests at maximum current.

Do not make the controller “explore” above a validated baseline.

### 3. Decide how reset diagnostics affect optimization

During tuning, correlate power changes with AWAKEN reset cause/hang zone. If BOD resets cluster at high power or low battery, define an explicit policy (exclude that node, cap power, or flag operator intervention). Do not silently infer BOD policy from a single reboot.

### 4. Close observability gaps

Confirm the dashboard/history makes these unambiguous per node:

- reported applied dBm and DYNAMIC/STATIC state;
- decision reason (`margin_low`, `headroom_step_down`, `silence_probe`);
- averaged margin and decision timestamp;
- command queued, acknowledged, timed out, or reconciled by STATUS;
- reset events between decisions.

## Validation matrix

| Scenario | Expected result |
|---|---|
| Strong clean link | Step down by 2 dBm no faster than once/minute, stopping at floor/dead band |
| Low margin | One jump to 13 dBm, not incremental recovery |
| Healthy uplink with ACK retries | No power increase based only on retries; step-down inhibited |
| STATIC operator pin | No automatic changes; STATUS reports static bit/value |
| Lost command ACK | Gate expires at 120 s; later STATUS reconciles actual state |
| Node reboot | Node/base return to 13 dBm DYNAMIC and old link counters are not differenced |
| Downlink lost but node running | Node stale-sync fallback returns to baseline |
| Uplink silent while downlink may work | Base sends bounded absolute baseline probes |
| Queue full/local send refused | No false in-flight state; decision may re-arm later |

Move this document to `Completed_Plans/` when constants have recorded evidence/rollback criteria, the ceiling decision is explicit, and the hardware matrix passes.
