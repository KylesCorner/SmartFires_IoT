---
name: lora-vs-lorawan
description: Why SmartFires uses a custom RadioHead/TDMA stack instead of LoRaWAN, mesh capability, CAD, and the optimization/range levers available on the current radio.
category: architecture
status: current
last_verified: 2026-06-26
source_refs:
  - platformio/src/platform/RadioHeadTdmaDriver.cpp
  - platformio/include/config/NetworkConfig.h
related_docs:
  - tdma-protocol
  - packet-reliability
  - tunable-parameters
  - dynamic-tx-power
---

# LoRa vs. LoRaWAN

SmartFires talks raw LoRa (RadioHead `RH_RF95` + a hand-rolled TDMA scheduler), not
LoRaWAN. This doc records why, what LoRaWAN would and wouldn't buy, whether mesh is on
the table, what Channel Activity Detection (CAD) actually is in this codebase, and the
optimization/range levers available on the current radio stack.

## Current radio configuration (baseline for everything below)

- **TX power:** fixed `13 dBm` (`NetworkConfig::kRadioTxPowerDbm`,
  `RadioHeadTdmaDriver.cpp:41`). No dynamic adjustment — see
  [dynamic-tx-power](../Pending_Plans/DYNAMIC_TX_POWER.md).
- **Spreading factor / bandwidth:** never explicitly set. `RadioHeadTdmaDriver::begin()`
  calls `setFrequency()` and `setTxPower()` but never `setModemConfig()`, so every node
  runs on RadioHead's `RH_RF95` default, `Bw125Cr45Sf128` (SF7, 125 kHz BW, 4/5 coding
  rate). This is implicit, not a chosen value — first thing to fix if SF ever becomes a
  tunable.
- **Frequency:** single channel, 915 MHz, no frequency hopping.
- **CAD:** not used — removed from the driver after confirming it was inert and
  redundant with TDMA's deterministic slotting. See below.
- **Collision avoidance:** entirely via TDMA slot assignment (`tdma-protocol`), not via
  any radio-level carrier sensing.

## Custom TDMA stack vs. LoRaWAN

| | SmartFires (custom RadioHead + TDMA) | LoRaWAN |
|---|---|---|
| Topology | Star: 1 base, N nodes, slot-scheduled | Star: gateway-centric, with multi-gateway diversity |
| Collision avoidance | Deterministic — compile-time slot per node (`slot = (NODE_ID-1) % NUM_SLOTS`), zero contention by construction | ALOHA-style — nodes transmit whenever they want; duplicate reception across gateways resolves collisions probabilistically |
| Reliability | Self-built: app-layer `ACK_SUMMARY` bitmap + retry window (`packet-reliability`) | Built-in Class A confirmed uplinks; ADR feedback carried in MAC layer downlinks |
| Rate/power adaptation | None today (fixed SF7 / 13 dBm) — see [dynamic-tx-power](../Pending_Plans/DYNAMIC_TX_POWER.md) | ADR (Adaptive Data Rate) is a first-class network-server feature |
| Regulatory duty cycle | Self-controlled exactly via TDMA slot timing; moot anyway since deployment is 915 MHz US (no airtime cap, unlike EU 868 MHz's 1%) | Network server enforces duty-cycle/dwell-time per region |
| Infrastructure | None beyond your own base Feather + Jetson | Normally a certified gateway + network server (ChirpStack/TTN), or self-hosted equivalent |
| Time sync | Already built — `TIME_SYNC` broadcast drives the shared session clock | Not part of the LoRaWAN spec; you'd still need your own sync layer for anything timing-sensitive |

**Decision: keep the custom stack.** For a single-base network where you control both
ends, the custom TDMA scheduler gets you something LoRaWAN structurally cannot:
collision-free deterministic slots, and Rx gating tied to a known schedule (the
[radio-rx-gating](../Completed_Plans/RADIO_RX_GATING.md) work depends on exactly this
determinism). LoRaWAN's main offering — ADR — is a few days of work to replicate on top
of telemetry the system already collects (see
[dynamic-tx-power](../Pending_Plans/DYNAMIC_TX_POWER.md)), not a reason to discard
working sync/reliability/Rx-gating infrastructure that's already shipped.

## Does LoRaWAN support mesh / node-to-node chaining?

No. LoRaWAN is strictly star-topology: end-devices only ever talk to gateways, never to
each other. A packet heard by multiple gateways is deduplicated by the network server —
that's reception diversity, not multi-hop routing. There is no node-to-node chaining
anywhere in the spec.

Multi-hop mesh (e.g., a node outside the base's range relaying through a closer node)
would require a *different* stack entirely — RadioHead's own `RHMesh`/`RHRouter`, or a
purpose-built mesh firmware like Meshtastic. That's a materially different and harder
architecture: routing tables, multi-hop slot scheduling (today's
`slot = (NODE_ID-1) % NUM_SLOTS` assumes every node hears the base directly), and relay
nodes burning extra battery forwarding other nodes' traffic.

**Not pursuing this** unless a concrete deployment shows up with nodes that genuinely
can't reach the base directly — it's a different project, not an incremental change on
top of the current TDMA design.

## What is CAD (Channel Activity Detection)?

CAD is a hardware feature of the SX1276 (the chip on the RFM95 module): the radio briefly
listens on its configured frequency/SF for an existing LoRa preamble before transmitting,
and backs off if it hears one. It's the LoRa-specific equivalent of CSMA carrier sensing.

In RadioHead, this lives in `RH_RF95::send()`
(`.pio/libdeps/.../RadioHead/RH_RF95.cpp:337`), which unconditionally calls
`waitCAD()` on every send. `waitCAD()` (`RHGenericDriver.cpp:78`) is a no-op — returns
`true` immediately — whenever `_cad_timeout == 0`, which is the default unless
`RH_RF95::setCADTimeout(ms)` is called to set a non-zero timeout.

`RadioHeadTdmaDriver::begin()` (`RadioHeadTdmaDriver.cpp:44-46`) has a commented-out
`setCADTimeout()` call, with a note that it was disabled "while debugging missed packets
between node and base." That's consistent with the mechanics above: enabling CAD with a
short timeout adds a real chance of a node backing off near a slot boundary or during
overlapping TDMA timing variance, producing exactly the missed-packet symptom the comment
describes — for no benefit, since TDMA already prevents collisions deterministically.

**CAD is correctly inert today, but the dead code should either be deleted or have its
rationale documented in-line** so a future change doesn't "fix" it back on without
realizing it's redundant with — and actively riskier than — the TDMA scheduler.

## Optimization opportunities

Roughly ranked by effort vs. payoff:

- **Make the modem config explicit.** Add an explicit `setModemConfig()` call in
  `RadioHeadTdmaDriver::begin()` and a named SF/BW/CR constant in `NetworkConfig.h`
  next to `kRadioTxPowerDbm`, instead of relying on RadioHead's implicit default.
- **Wire up dynamic TX power.** The base already has the signal it needs — per-frame
  RSSI and the lifetime `retx_total`/`fail_total` counters in every `STATUS` packet —
  and isn't using either to adjust anything. See
  [dynamic-tx-power](../Pending_Plans/DYNAMIC_TX_POWER.md).
- **13 dBm leaves real headroom unused.** `setTxPower(13, false)` already targets the
  PA_BOOST pin (correct for RFM95), which supports up to 20 dBm through RadioHead, and US
  915 MHz ISM rules allow more than that. Raising the baseline costs little extra battery
  given TX is only ~340 ms of a 3,600 ms frame (~9% duty cycle) and buys margin everywhere
  for free.
- **Resolve the dead CAD code** per the section above — delete it or document why it
  stays disabled, so it isn't silently re-enabled later.
- **Slot width is sized for one implicit SF.** `kSlotWidthMs = 900 ms` derives from
  `kBundleTxBudgetMs = 340 ms` at the current (implicit) SF7. Any future move to a higher
  SF — including range "boost mode," below — has to reckon with this: time-on-air roughly
  doubles per SF step, so a 15-sample bundle that fits in 340 ms at SF7 will not fit in a
  900 ms slot at SF10+.
- **No regression test exists for "the base only ever transmits in slot 0."**
  [radio-rx-gating](../Completed_Plans/RADIO_RX_GATING.md) already flags this as a silent-
  failure risk for the Rx-gating feature that's shipped; it becomes more important the
  more radio-config commands get added that also depend on that invariant.

## Maximizing range on demand

Three independent levers, in order of how cheaply they buy range:

1. **Raise TX power first.** 13 dBm → 20 dBm is +7 dB for the cost of a few extra mA only
   during the ~340 ms TX window per frame — the cheapest range win available.
2. **Raise SF when power alone isn't enough.** SF7 → SF12 at BW125 buys roughly 14 dB of
   receiver sensitivity (~-123 dBm → ~-137 dBm) — several times the usable distance in
   clear line-of-sight, and more valuable still in forested/hilly terrain where the link
   is diffraction/obstruction-limited rather than purely free-space. The cost is airtime:
   a 15-sample `BUNDLE` that fits in 340 ms at SF7 will not fit at SF10+ without either
   shrinking the payload or widening the slot.
3. **Treat range-boost as an on-demand mode, not a steady state.** Given the slot-width
   conflict in #2, a future "boost mode" should be a narrow downlink command — parallel to
   the existing `CMD_CALIBRATE`/`CMD_RESET` round-trip — that temporarily pushes a single
   node to high-SF/high-power *and* drops it to `STATUS`/`AWAKEN`-only traffic (no
   `BUNDLE`) until it's confirmed back in range. That avoids redesigning slot widths for
   what should be a rare case (a drifted-out-of-range node, or deliberate max-reach
   testing), rather than permanently shrinking everyone's throughput to cover an edge case.

A field antenna/placement check (mount height, polarization match between node and base
whips, line-of-sight) will often move the needle more than any radio-parameter change —
worth ruling out before chasing SF/power further.
