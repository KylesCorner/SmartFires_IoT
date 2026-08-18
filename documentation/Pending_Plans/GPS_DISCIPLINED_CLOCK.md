---
name: gps-disciplined-clock
description: Plan to give each node a continuously-running SAMD21 RTC COUNT32 timebase (replacing Arduino core millis() plus sleep-compensation), then periodically discipline that counter's tick rate using the PA1010D GPS's 1 Hz PPS edge. Two independent steps — Step 1 has no GPS dependency at all.
category: plan-pending
status: draft
related_docs:
  - rtc-subsecond-sleep
  - tdma-protocol
  - duty-cycling
---

# GPS-Disciplined Node Clock

## Background

Today a node's timekeeping runs through two separate mechanisms:

- **Local tick source.** `ArduinoClock::millis()` returns `::millis() + _sleepOffsetMs` — the
  Arduino core's own tick source while awake, with an offset patched in by
  `Samd21RtcSleep::sleepFor()` after every MCU standby (measured off the SAMD21 RTC's
  MODE0/COUNT32 counter at 1024 Hz, per `[[rtc-subsecond-sleep]]`). The RTC is already precise
  and already free-running through standby — it's just only *consulted* around sleep, not used
  as the clock itself.
- **Cross-node epoch.** `TdmaClock::sessionNowMs()` is `syncSessionMs + (millis() - syncLocalMs)`.
  The base broadcasts `TIME_SYNC` over LoRa every 10 minutes; between syncs, slot alignment
  depends entirely on how accurately local `millis()` tracks real elapsed time. `NetworkConfig.h`
  sizes `kGuardMs = 20` explicitly to cover "crystal drift between 10-min sync intervals at
  50 ppm" — the guard band exists because nothing corrects that drift between syncs.

GPS was flagged as a fix for this and then explicitly deferred: `SOFTWARE_DESIGN.md`'s key
decisions record *"TIME_SYNC driven by Jetson NTP, not GPS. GPS PPS sync deferred; current
crystal drift between synced nodes is within the 20 ms guard band."* A fuller version of the
idea was scoped in `TDMA_BUNDLE_SIZING.md` ("GPS-Disciplined TDMA"): wire the PA1010D's PPS pin
to the Feather, use it for sub-ms sync, shrink the guard band from 20 ms to 2–3 ms. That version
also requires **a PPS reference on the base station**, which is a bigger hardware and protocol
commitment (and doesn't help indoor nodes, which have no sky view at all — see
`project_scope_indoor`).

This plan is the narrower version: GPS *periodically* corrects each node's own local counter;
it does not become the cross-node epoch authority. `TIME_SYNC` over LoRa stays in charge of
"what time is it, network-wide" — GPS just makes the node's own counter tell better local time
between syncs. This needs no base-station hardware and degrades to exactly today's behavior on
any node with no fix (indoor deployments included).

One fact from the real firmware that shapes Step 2: the production node build
(`src/main.cpp`, the real `feather_m0_lora_node`/`_node_debug` construction, not the
`POWER_TEST_MODE_GPS` bench build) configures the GPS as `GpsPowerMode::Backup`, not
`FullPowerContinuous`. Per `Pa1010dGpsSensor::begin()`'s own comment, wake/sleep timing for
`Backup` mode is driven by the app-level `DutyCycleController`, not any GPS-internal periodic
schedule. In other words, **GPS (and therefore PPS) is only up during the node's normal duty-cycle
wake window today** — the same window used for every other duty-cycled sensor. Any discipline
scheme has to work inside that reality, not assume a standing PPS signal.

## Goal

Make each node's local millisecond clock more accurate than its raw crystal tolerance, using
GPS PPS as an occasional correction source, without changing what's authoritative for network-wide
time (`TIME_SYNC`) or touching indoor/no-fix node behavior.

## Non-goals (this plan)

- No base-station GPS/PPS hardware. `TIME_SYNC` remains the epoch authority.
- Not shrinking `kGuardMs` yet. That's a follow-on decision made from field data on how tight
  the corrected clock actually gets — this plan produces that data, it doesn't spend it.
- No change to `Hybrid` duty-cycle mode, consistent with `[[rtc-subsecond-sleep]]` treating it
  as deprecated.

---

## Step 1 — Unify the live timebase on RTC COUNT32 (no GPS involved)

### What

`ArduinoClock` stops wrapping `::millis()` and instead reads `RTC->MODE0.COUNT` continuously
(converted via the existing `Samd21RtcTicks::ticksToMs()`), both while awake and asleep. The
counter never stops, so there's no gap to patch after a wake — `ArduinoClock::compensateForSleep()`
and its `_sleepOffsetMs` field go away entirely, and `Samd21RtcSleep::sleepFor()` no longer needs
to call back into `ArduinoClock` at all; it can just read the same `COUNT` register directly for
its own elapsed-time bookkeeping.

### Why on its own, before any GPS work

- Removes a whole class of error that GPS discipline wouldn't touch anyway: today, awake-time
  is measured by whatever clock tree backs the Arduino core's `millis()`, which is a different
  (and not necessarily as stable) domain from the crystal-backed RTC used for sleep. Unifying
  onto one counter is a strict simplification independent of GPS.
- It's the literal shape of "the base timer on the node is just a count32" — this step alone
  delivers that, and is useful/testable before Step 2 exists.
- Every consumer goes through `IClock::millis()` already (`TdmaClock`, `DutyCycleController`,
  `PacketHandler` timestamps, `SmartFiresNodeApp`), so the interface contract doesn't change —
  this is a backing-implementation swap, not a call-site migration.

### Design notes

- `Samd21RtcTicks.h` stays hardware-free/pure (deliberately unit-testable on `native` today —
  keep that property). The wraparound caveat already documented there ("exact only up to
  ~48.5 days") is no worse than today's `u32` `millis()` wraparound (~49.7 days) — worth an
  explicit note in the rewrite, not a new problem.
- `Samd21RtcSleep::sleepFor()` shrinks: no more `_clock.compensateForSleep(elapsedMs)` call,
  since "the clock" and "the sleep-elapsed measurement" become the same register read.
- This step is a pure prerequisite for Step 2, but ships value on its own even if Step 2 never
  happens — it's fine to land, bake, and evaluate independently.

### Verification gate before Step 2

- `pio test -e native` — existing `test_rtc_ticks` suite should be untouched (pure math, no
  interface change); add coverage for the new `ArduinoClock` behavior via a fake/mock RTC read
  if the current test harness supports it, otherwise note as hardware-only.
- Bench/field bake on `feather_m0_lora_node_timed` (isolated from the production env, same
  pattern `[[rtc-subsecond-sleep]]` used): confirm `sessionNowMs()`/slot timing behave
  identically to pre-change baseline — this step should be a no-observable-behavior-change
  refactor from the TDMA protocol's point of view. Any deviation means the unification isn't
  actually equivalent and needs to be understood before Step 2 builds on top of it.

---

## Step 2 — GPS PPS rate discipline (depends on Step 1)

### What

Whenever the GPS is awake (its normal `DutyCycleController`-driven wake window, per Background)
and reports a fix, capture successive PPS edges and use the fact that they are exactly 1.000000 s
apart to measure the true tick rate of the COUNT32 domain. If two edges are 1026 ticks apart
instead of the nominal 1024, the crystal is running fast by a known, measurable amount — apply
that as a correction factor to `ticksToMs()`/`msToTicks()` so the *node's own* elapsed-time math
gets more accurate, independent of how good or bad that particular crystal actually is.

This is rate (frequency) discipline, not epoch discipline. A one-off "snap to GPS second
boundary" doesn't help — drift resumes immediately after the snap. Continuously re-measuring the
true rate and correcting for it is what actually shrinks the error that accumulates between
`TIME_SYNC` broadcasts.

### Design decisions

**Capture mechanism.** PA1010D PPS pin → a Feather M0 GPIO with EIC (external interrupt)
capability → ISR does an immediate `COUNT` read via the existing `READREQ` path (already used by
`Samd21RtcSleep::readCount()`). The existing code already documents ~1–2 tick sync latency on
that read path, which cancels in deltas the same way it does for sleep timing today — fine at the
millisecond precision this plan targets. A hardware event-capture path (EVSYS-routed, no ISR
involved) would be tighter, but isn't worth the added complexity unless bench data shows the
ISR-latency approach isn't good enough.

**Fix-quality gating and averaging.** A single PPS interval is noisy (interrupt latency, the RTC's
own ~1-tick read quantization). Gate any correction attempt on `fix=true` (and probably a minimum
satellite count), and average/low-pass-filter across many consecutive edges — tens of seconds to
a few minutes — before updating the correction factor, rather than snapping to each new edge.
Given GPS is only up during the normal sensing wake window (Backup mode, `DutyCycleController`-
driven), the discipline attempt is naturally bounded to however long that window already is; if
that's too short to gather enough edges for a stable estimate, that's a real constraint to measure
early rather than assume away.

**Where the correction state lives.** `Samd21RtcTicks.h` stays pure/stateless (unit-testable on
`native`, as today) — the nominal `kTicksPerSecond = 1024UL` remains the no-fix fallback. A new,
small class owns the mutable, GPS-derived correction factor and calls into the existing pure
tick-math functions rather than mutating them. That keeps the core math testable with synthetic
PPS-interval sequences (feed known tick counts, assert the filter converges to the expected
correction) without touching hardware in the test.

**Persistence.** RAM-only, recalibrated from scratch after every reset — the same pattern already
accepted for the IMU's DMP compass calibration ("biases are RAM-only and lost on power cycle").
Run nominal 1024 Hz until the first disciplined window completes after boot; no flash-persistence
complexity unless field data says the bootstrap-accuracy gap actually matters.

**Interaction with `TdmaClock` / the guard band.** None, deliberately, in this step. The corrected
counter feeds `ArduinoClock::millis()` (via Step 1's plumbing) exactly like the uncorrected one
does today — `TdmaClock` doesn't need to change at all. `kGuardMs` stays at 20 until there's field
evidence of how tight the corrected clock actually runs; shrinking it is explicitly a possible
*future* plan, not part of this one.

**Hardware.** PPS pin needs wiring from the PA1010D breakout to an EIC-capable Feather M0 GPIO —
needs a schematic/pinout check to confirm which pin is free and EIC-capable (open question below).

### Verification / bake plan

- Native unit tests for the correction-factor class: synthetic PPS-interval tick sequences (both
  clean and jittery) → assert convergence and that a nominal (no-fix) fallback is used until the
  first valid window completes.
- Bench: log both the raw (uncorrected) and GPS-corrected elapsed-time estimate side by side
  against a known-good external reference for an extended period, on `feather_m0_lora_node_timed`.
  Confirm the corrected estimate is actually better before trusting it anywhere near the TDMA path.
- Field: gate behind a compile flag so it can be disabled without a full fleet reflash if the
  correction misbehaves. Log correction-factor value and confidence (e.g., edges-since-last-update)
  as a debug line so field behavior is inspectable the same way `wake_phase_err` was for
  `[[rtc-subsecond-sleep]]`.
- Indoor/no-fix sanity check: confirm a node that never gets a fix behaves identically to a
  pre-Step-2 node — this should require no special-casing if the fallback design above is right,
  but worth an explicit test since it's the deployment path that matters most for
  `project_scope_indoor`.

---

## Sequencing vs. in-flight work

`[[rtc-subsecond-sleep]]` Phase 2 (T6 + the `PktHeader` flags-byte wire break) is coded but
**unflashed** as of this writing. It touches the same files this plan needs to build on
(`Samd21RtcSleep`, the sleep/TDMA-resync interaction). Land and bake that first — don't stack an
unvalidated wire-format break and an unvalidated clock-source change in the same flash cycle, or
a field problem afterward is much harder to attribute to either change.

Recommended order:
1. `[[rtc-subsecond-sleep]]` Phase 2 flashed, baked, confirmed stable.
2. This plan's Step 1 (timebase unification) — no GPS, no wire changes, low risk, independently
   baked.
3. This plan's Step 2 (GPS PPS discipline) — builds on Step 1, its own bake/verification pass.

---

## Open questions

- **Primary motivator still undecided:** tighter TDMA guard band / more nodes per frame, vs.
  cross-node timestamp correlation accuracy for the fire-stage classification study
  (`[[project_first_study]]`). Both benefit from Step 2, but they imply different "good enough"
  bars and would affect whether a future base-station PPS reference (the fuller
  `TDMA_BUNDLE_SIZING.md` version) is ever worth revisiting. Not blocking Step 1 or Step 2, but
  worth settling before deciding whether to ever touch `kGuardMs`.
- **PPS pin selection:** which Feather M0 GPIO is free, EIC-capable, and reachable from the
  PA1010D breakout's PPS pad — needs a schematic/pinout check, not yet done.
- Whether the `Backup`-mode wake window is long enough to gather a stable rate estimate, or
  whether the discipline attempt needs its own, separately-tunable wake cadence distinct from
  the sensing duty cycle (would mean touching `SensingConfig::Gps` and possibly GPS power
  budget — currently untouched by this plan).
