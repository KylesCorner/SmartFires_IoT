---
name: network-system-design
description: Historical development log for the single-board, binary telemetry, TDMA, reliability, RX-gating, and watchdog decisions.
category: plan-completed
status: historical
related_docs:
  - software-design
  - packet-reliability
  - tdma-protocol
---

# Dev Update Log — Data Transmission & System Design

> Historical development narrative. Values and packet sizes below describe successive implementation stages and are not a current reference; use `SOFTWARE_DESIGN.md` and `Current_Architecture/` for shipped behavior.

## Issue: Node Architecture (Dual-MCU → Single Board)

Problem Description: The original node design used two MCUs: an ESP32 that owned the sensors and packet generation, connected over UART to a Feather M0 that owned the LoRa radio. This meant two firmware stacks, a UART bridge protocol with its own ACK semantics, and a mismatch where the UART link was much faster than the LoRa link — so backlog and rate-control policy had to live on the Feather anyway. Debugging a dropped reading meant figuring out whether it died at the sensor, the UART hop, or the radio hop.

Solution: We consolidated to a single-board node (“only-feather” refactor): one Feather M0 owns both sensing and LoRa transmission. This eliminated the inter-board UART bridge entirely, cut one failure point out of every packet’s path, and let us restructure the firmware into a class-based architecture (SmartFiresNodeApp → PacketHandler → TdmaRadioService → radio driver) with unit tests that run on desktop against fake clocks/radios/sensors.

## Issue: Bandwidth

Problem Description: Sending raw sensor data in each packet as-is costs significant bandwidth. Our first telemetry format was ASCII CSV-style text — every packet re-sent every field, unchanged or not, as human-readable text (~90 bytes per single sample). At LoRa airtime rates this was untenable.

First Step — Binary Protocol: We replaced text with a binary wire format: a 4-byte packed header (magic / packet type / node id / sequence), fixed-point integer fields instead of floats (e.g. temperature as centi-°C in an int16, wind as cm/s in a uint16, lat/lon as degrees × 1e7 in int32), and a CRC-8 over the payload. Fixed-point integers give deterministic sizes and avoid float/printf issues on the MCU. A full-state sample dropped from ~90 bytes of text to 36 bytes of binary.

Second Step — Delta Bundles: To go further, each packet became a bundle: one full-precision reference frame plus subsequent delta frames. The delta for each sensor requires less precision/fewer bytes (e.g. temperature delta as a single signed byte in 0.1 °C steps), so many samples fit in one LoRa transmission. Sending one large packet is also far cheaper in airtime than many small ones, because every LoRa transmission pays a fixed preamble/header cost.

Iteration that didn’t survive: the first delta payload was 16 bytes and bundles carried 7 deltas (8 samples in a 141-byte payload, ~17.6 bytes/sample). We later compacted the delta to 12 bytes and raised the ceiling to 14 deltas — 15 samples in a ≤194-byte payload, ~13 bytes/sample, a ~7× improvement over the original text format. Each delta carries a clamp/overflow flags byte so the receiver knows when a delta saturated its small field; the periodic reference frame (like an I-frame in video) bounds how long any decode error can propagate.

GPS was a sub-problem of its own. First we sent lat/lon in every packet (8 wasted bytes when stationary). Then we tried a one-shot PKT_GPS sent once per session — but a single lost packet meant no location for the entire session, and session restarts had to trigger re-sends. The design that stuck is a periodic PKT_STATUS packet (GPS + battery + heading + link stats, with validity flags for each field) so location is self-healing without riding in every telemetry packet.

We also derived the math for this (documented in TDMA_BUNDLE_SIZING / BANDWIDTH_SCALING): closed-form LoRa airtime by delta count, the RadioHead 251-byte payload ceiling, and the queue-pressure ratio η = frame time / bundle accumulation time. η > 1 means nodes produce bundles faster than the TDMA schedule can drain them, and the steady-state loss rate is (η−1)/η. This math predicted that adding a third node at our original 4 Hz sensing rate would silently drop ~26% of bundles — which is why we halved the sensing rate rather than discovering the loss in the field.

## Issue: Packet Loss & Reliability

Problem Description: LoRa is a radio physical layer and does not guarantee delivery. Our early method was pure fire-and-forget: send and hope the base station hears it. In a controlled environment with one node we observed >90% reception, which was satisfactory. When we deployed two nodes in a live environment, reception fell to ~50–60% per node.

Diagnostic detour (tried, useful, but not kept): at the worst point we couldn’t even tell whether the problem was the radio link or the node’s sensing pipeline — data was simply missing. As a diagnostic tool we temporarily forced *every* uplink through RadioHead’s blocking link-layer ACK (`sendToWait`) so the logs showed `link_ack=OK/NO` per packet. This proved the nodes were sampling and transmitting fine and the loss was on the RF link — but it was never viable as the production design.

Initial Plan: Keep per-packet link-layer ACKs (RadioHead’s `waitForAck`/`sendToWait`) as the reliability mechanism: sender waits an interval for an acknowledgement and retries several times before giving up. This did get packets through, but each packet then occupies up to (transmit time + ACK timeout) × attempts of channel time — with a 250 ms ACK timeout and 3 retries, one unlucky bundle could eat most of a TDMA slot. Effective bandwidth dropped significantly, and the node was blocked (not sensing, not servicing its queue) while waiting.

Second Plan and Solution: We built a custom app-layer reliability mode (AppLayerAckSummary) that moves acknowledgement off the per-packet critical path:

- A node transmits telemetry fire-and-forget with no ACK wait, then stores a copy in a pending window (ring buffer, 8 entries deep).
- The base station tracks received sequence numbers per node and periodically replies with a compact ACK_SUMMARY packet: a base sequence number plus a 16-bit bitmap covering the following sequences — 17 packets acknowledged in a 9-byte packet.
- When a node receives an ACK_SUMMARY, acknowledged packets are freed from the pending window; anything still unacknowledged becomes eligible for retransmission during otherwise-idle TX time in later slots.

Tuning that took iteration to get right:

- **ACK-paced retry gate.** Early versions would retransmit a packet before the base had even had a chance to acknowledge it, wasting airtime on packets that had actually arrived. We added a retry-wait derived from the expected ACK_SUMMARY cadence (~2× the expected 4 s interval, clamped to 4.5–10 s) so a pending packet is only retried after an ACK summary covering it should have arrived.
- **Fresh-traffic priority.** Fresh telemetry always outranks retransmits; retries only fill idle slot time, and are suppressed for 2 s after any fresh send. We also experimented with reserving a dedicated retry slot per TDMA frame before settling on opportunistic idle-time retries.
- **Bounded effort.** Pending entries are dropped after 3 retransmit attempts or 30 s of age — stale telemetry is not worth more airtime than fresh telemetry.
- **Stopping redundant link ACKs.** The base’s RadioHead receive path was still auto-ACKing every fire-and-forget telemetry packet at the link layer — acknowledgements nobody was listening for. We stopped that to reclaim the airtime.

Control-plane packets (AWAKEN, direct TIME_SYNC replies, ACK_SUMMARY itself) still use link-layer ACKs — they’re small, rare, and boot/reliability-critical, so the blocking cost is acceptable there. The other key boundary decision: reliability lives entirely between the two Feathers. The Jetson ingests and logs but is not part of the acknowledgement loop, so radio-link recovery never depends on the serial link or Jetson process state.

## Issue: Packet Collision / Packet Loss

Problem Description: A co-issue to the reliability problem above — much of the two-node loss was simultaneous transmissions, nodes talking over each other on the shared 915 MHz channel.

Initial Plan (tried and abandoned): LoRa radios support Channel Activity Detection (CAD) — listen before transmit, only send if the channel is clear. We added a CAD check ahead of every transmission. It helped, but created new problems: nodes would “fight” for the channel, backoff added dead time before each transmission, and — decisively — CAD is not resilient to the hidden-node problem. If the base station sits between two nodes that are each beyond half their radio range from the other, they can both reach the base but cannot hear each other, so both see a “clear” channel and collide at the base anyway. That topology is exactly our deployment geometry.

Second Plan and Solution: TDMA — time is divided into repeating frames of NUM_SLOTS equal slots, and every transmitter (including the base station) owns exactly one slot. Slot 0 is permanently reserved for the base; nodes get slots from their assigned IDs. Collisions become structurally impossible rather than probabilistically avoided, and bandwidth is shared equally by construction. Slot width is derived from the math above: worst-case bundle airtime + ACK budget + a 20 ms guard band per edge sized against crystal drift between time syncs.

A bug we had to find ourselves: the base station was originally exempt from its own schedule — it transmitted TIME_SYNC and ACK_SUMMARY whenever they were ready, at arbitrary phase, stepping on node slots. Fixing this meant giving the base its own TDMA clock and deferring every base-originated send until slot 0 opens.

Debugging tooling: to see any of this, we built a passive LoRa sniffer (a dedicated Feather flashed with monitor-only firmware) feeding a TDMA timeline visualization on the web dashboard, which renders each packet against the slot grid (“piano roll” view) with per-slot jitter stats. Most of the slot-boundary bugs above were found by looking at that timeline, not at serial logs. This is coupled with an SDR which allows us to passively visualize the transmissions.

## Issue: TDMA Session Info (Node ID and Time Sync)

Problem Description: TDMA requires each node to hold a unique ID within the slot count, and all nodes to share a clock so they know when their slot is active.

Initial Solution to Node ID: Hard-coded node IDs as build flags at flash time. Worked, but cumbersome — every node needed its own build environment, and a cloned flash meant two nodes silently sharing a slot. Not a long-term answer.

Initial Solution to Time Sync: The base station broadcasts a session time which nodes use to sync their internal clocks.

Second Plan and Solution: Both pieces of information now come from an Awaken handshake. On boot, a node broadcasts an AWAKEN packet and holds its sensors idle — no telemetry is generated until sync arrives, which guarantees every sample carries a valid session timestamp from the first reading. The base replies with the node’s TDMA slot assignment and current session time. Periodic TIME_SYNC broadcasts (fire-and-forget — a missed one is superseded by the next) correct clock drift thereafter; the 20 ms guard band absorbs drift between syncs. If sync goes stale (>22 min without a TIME_SYNC), nodes fall back to transmitting immediately rather than going silent forever.

Failures along the way:

- **Broadcast assignment reassigned everyone.** The first version of the assignment reply was received by *all* active nodes, each of which adopted the new ID: one node booting could re-slot the whole network. This is what forced adding the node’s unique serial number to the AWAKEN packet: nodes derive a 32-bit hash (uid_hash) of the MCU’s 128-bit hardware serial, the base addresses the assignment to that specific hash, and only the matching node applies it. This also solved the hard-coded-ID cloning hazard permanently.
- **AWAKEN contaminating the telemetry path.** AWAKEN packets initially flowed through the normal telemetry queue, where they could sit behind buffered bundles and confuse the sequence tracking. We moved AWAKEN handling out-of-band and made the base explicitly reject AWAKENs arriving via the normal queue.
- **Sequence-tracker resets.** A node rebooting (new AWAKEN, sequence numbers restarting at 0) initially looked like massive packet loss to the base’s and dashboard’s gap-based loss trackers. AWAKEN now resets the per-node sequence tracker on the base and the web history.

The base keeps the serial-number → node ID mapping for the session, so sensor data and link performance can be tied to a specific physical unit across runs.

## Issue: Session Management

Problem Description: Our initial design had the Jetson own session management for the LoRa network — session time, node ID assignment, and so on — with the base station Feather acting as a dumb serial-to-radio bridge. This broke whenever the components didn’t boot in the right order or a session was restarted on the Jetson: nodes would AWAKEN into a base station that couldn’t answer until the Jetson process was up, and a Jetson restart could invalidate state that live nodes were still operating on.

Solution: We moved session logic down to the base station M0. The base answers AWAKENs immediately from its own state, owns slot assignment, and generates ACK summaries locally; none of it waits on the serial link. Timekeeping was reduced to a single clock: a session-milliseconds counter maintained on the base Feather. That’s all nodes ever see; mapping session time onto real wall-clock time happens once, at data ingestion on the Jetson. The Jetson still injects TIME_SYNC (NTP-derived) when present, but the system degrades gracefully to the base’s local clock if it isn’t.

Related hardening from the same problem family: the Jetson’s serial ingest got reconnect-with-backoff so a base station USB drop doesn’t kill the session; the base moved to native USB CDC with stable udev-symlinked device paths (the base and sniffer share a VID/PID and would otherwise swap ttyACM numbers between boots); and the web “New Session” action was scoped down to clearing Jetson-side data only, after we found that having it also reset nodes and the base created exactly the out-of-order-reboot races we’d just designed away.

## Issue: Radio Power (RX Gating)

Problem Description: With telemetry fire-and-forget, a node’s receiver has nothing to do most of the time — the base only ever transmits in slot 0 — yet the radio sat in continuous RX, one of the largest constant power draws on the node.

Solution and what broke: We gate the receiver: the radio sleeps outside the base’s slot 0 window (projected ~56% cut in radio current). The first implementation gated the window too tightly to slot 0 itself: in field testing, nodes were still finishing a blocking sensor read when slot 0 began, woke the radio late, and missed the start of the base’s transmission surfacing as base-side link-ACK retries and missed TIME_SYNCs. The fix was a dedicated wake-ahead margin (rxWakeAheadMs, 50 ms) that opens the RX window before slot 0 - sized to absorb main-loop jitter from blocking sensor reads, which is a different and larger error source than the crystal drift the guard band covers. It took two rounds of extending this pre-listen before a 3-node network ran cleanly. The gate also mirrors the TDMA fallbacks: RX stays continuously open before first sync and whenever sync goes stale, so a power optimization can never lock a node out of rejoining.

## Issue: Node Hangs (Unrecoverable Freezes)

Problem Description: Field reports came in of nodes that stopped responding entirely — no telemetry, no AWAKEN retries, no debug log activity — recovering only after someone drove out and power-cycled the board. This is a single-threaded, non-preemptive-by-us architecture (no RTOS), so any blocking call that never returns takes the whole main loop down with it, silently.

Diagnosis, working from real device logs across several sessions rather than code inspection alone, turned up four concrete mechanisms:

- **Auto-ACK-on-receive.** RadioHead's `RHGenericDriver::waitPacketSent()` has a no-timeout overload: `while (_mode == RHModeTx) YIELD;`. `_mode` only clears from the DIO0 interrupt handler, and DIO0 is edge- not level-triggered — the vendored library's own source comments admit a "slim chance of missing events." A missed edge means the wait spins forever. A device log caught this directly: the node's automatic ACK of an `ACK_SUMMARY` packet hung for ~115 s before recovering on its own, on a call path that had already succeeded hundreds of times earlier in that same session — a genuine wait-condition stall, not memory corruption.
- **`RH_RF95::send()`'s own leading wait.** Easy to miss because `send()` looks like fire-and-forget: it opens with the same unbounded `waitPacketSent()`, but waiting on *whatever transmission came before it*, not its own. Every node telemetry send goes through this. A second, structurally distinct incident confirmed it: three consecutive `retx_blocked` log lines (pure bookkeeping, no radio I/O) followed by ~115 s of silence, then the node came back having lost TDMA sync entirely — a bigger operational hit than the hang itself, since resyncing meant redoing the AWAKEN handshake, which runs through one of the still-open mechanisms below.
- **`RHReliableDatagram::sendtoWait()`'s internal wait.** This one isn't fixable at the call site: `sendtoWait()` is a single opaque vendored call running its own retry loop, with the unbounded wait buried inside it. There's no seam to intervene from our code between attempts. Reimplementing the retry protocol ourselves was considered and rejected — the correctness of that protocol depends on private state (running sequence number, retransmit counter, per-sender duplicate-ID table) with no accessors, and getting any of it subtly wrong trades a loud, obvious hang for a quiet correctness bug (wrong ACK accepted, a retry not deduped) that's far harder to catch. Patching the vendored `.cpp` directly was also rejected: it's a PlatformIO-fetched dependency, not something checked into this repo, so a hand-edit evaporates on a clean build or `pio lib update` — durable would mean forking RadioHead, a standing maintenance commitment we weren't ready to take on for one bug fix. Six call sites carry this exposure across node and base; the highest-frequency is the base's own periodic `ACK_SUMMARY` send (hundreds of times per multi-hour session), with the node's AWAKEN/TIME_SYNC handshake a close second in importance, since it's specifically the recovery path taken right after something has already gone wrong.
- **Shared I2C bus, no timeout.** SHT31, ICM-20948, and the GPS all sit on one `Wire` bus with no bus-recovery sequence anywhere in the firmware. One confirmed field hang traced all the way down to `SparkfunIcm20948Driver::read()` → `SERCOM::readDataWIRE()`'s bare `while (sercom->I2CM.INTFLAG.bit.SB == 0) {}` — no timeout, tripped by a slave that never completed a byte handshake. This one remains fully open; no bus-recovery routine (e.g. toggling SCL to unwedge a stuck slave) has been attempted yet.

The first two were fixed directly: the node now disables RadioHead's automatic ACK (`autoAck=false`) and acknowledges explicitly through a hand-rolled path that waits for its own transmission via the *bounded* `waitPacketSent(timeout)` overload instead of the unbounded one, and `RadioHeadTdmaDriver::send()` got the same bounded-wait treatment. Worth recording because it wasn't obvious on the first attempt: the very first version of the ACK fix removed the wait entirely instead of bounding it, on the reasoning that if an unbounded wait caused the hang, no wait at all must be safe. It wasn't — nothing then stopped the RX-power-gating `sleep()` call from commanding the radio to sleep mid-transmission, since `RH_RF95::sleep()` has no guard against an in-flight send, and this surfaced as the base retransmitting `ACK_SUMMARY` far more than expected. Bounding the wait, not removing it, is what actually closed the gap.

The other two — RadioHead's internal retry wait, and the I2C bus — have no equivalent call-site fix. That gap is what the watchdog timer below exists to close.

## Issue: Watchdog Timer (Automatic Recovery from Unfixable Hangs)

Problem Description: Two of the four hang mechanisms above can't be fixed at the call site — not because it hasn't been attempted, but because the alternatives are worse. Reimplementing RadioHead's retry protocol trades a loud hang for a quiet correctness bug; patching the vendored dependency doesn't survive a clean build; a hand-rolled interrupt-based "cancel this blocked call" scheme still ends up needing to reach into driver internals with the same corruption risk as the sleep-race bug above — at which point it's just a narrower, more fragile reimplementation of a hardware watchdog. Something general-purpose that doesn't need to know which subsystem wedged was the better trade.

Solution: Arm the SAMD21's built-in hardware WDT (via `Adafruit_SleepyDog`) rather than build a software heartbeat watchdog — a software watchdog is just more code that can hang under the exact conditions it's meant to guard against. The SAMD21's WDT runs off its own independent ~32 kHz oscillator, so it keeps counting even if the CPU is dead inside a stuck loop; when it fires, it's a genuine hardware reset, equivalent to a power cycle, which resets the radio chip's actual hardware state too (`RadioHeadTdmaDriver::begin()` already toggles the RFM95's hardware RST pin), not just the MCU's software model of it.

Timeout is split into two tiers rather than one constant, because the boot sequence (5 s fixed delay, serial wait, I2C scan, sensor `begin()`) is already close to the SAMD21's ~16 s hardware ceiling on its own: a longer boot-phase timeout is armed at the very top of `setup()`, then re-armed to a shorter ~8 s steady-state timeout right before `loop()` begins — comfortably more than double a full TDMA frame, so no normal iteration comes close to it. Petting happens at the existing `Samd21RamMonitor` checkpoint boundaries in `setup()` (a diagnostic logger already there for an unrelated purpose, but conveniently checkpointed at almost exactly the boundaries the watchdog needed too) and once per `loop()` iteration — deliberately *not* inside any of the six `sendtoWait()` call sites or any I2C driver call, since those are exactly what the WDT needs to still be able to catch, and not inside the existing infinite `while(true){delay(500);}` retry loop on `begin()` failure, so that loop now self-resolves via reboot instead of spinning forever. Whether a reset was WDT-triggered is logged at boot from the reset-cause register, so a self-healed hang stays visible in the debug log instead of quietly looking like a routine power-on.

One register-level bug surfaced during implementation and is worth recording: the initial plan referenced `RSTC->RCAUSE`, which is the reset-cause register on SAMD51/SAME51 parts. The Feather M0's SAMD21 exposes reset cause through the Power Manager peripheral instead — `PM->RCAUSE.reg` with bit `PM_RCAUSE_WDT` — confirmed against the actual CMSIS headers PlatformIO installs for this board before writing the final code. A good example of why register-level assumptions in a plan doc still need verifying against the real toolchain, not just carried over from a similar-sounding chip family.

Both the node and the base station get this treatment — the base has its own infinite `begin()`-failure loop and the identical `sendtoWait()` exposure on its three outbound calls, and its `ACK_SUMMARY` send is in fact the single highest-frequency instance of that exposure anywhere in the system. Deferred: surfacing a lifetime WDT-reset count to the Jetson (would ride along in `STATUS` the way `retx_total`/`fail_total` already do) — worth its own design pass once field data shows whether WDT trips are rare or frequent enough to need remote visibility. No native/desktop test coverage is possible for the WDT itself, since it's a real hardware peripheral with no software fake; verification is bench-only (confirm no false-positive reboots over a full session, confirm an injected `while(true);` actually gets caught within the configured window, confirm the boot-phase timeout — not the shorter steady-state one — is what's active during a deliberately stalled I2C bus at boot).
