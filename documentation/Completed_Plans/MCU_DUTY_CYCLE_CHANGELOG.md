---
name: mcu-duty-cycle-changelog
description: Changelog and implications review of commit d7ba3c5 (branch feature/mcu-duty-cycle) — adds SAMD21 RTC standby, a Timed/Hybrid DutyCycleController mode, and radio duty-sleep gating; flags that every RTC wake forces a full TDMA resync (fresh AWAKEN handshake) and that RTCZero's whole-second alarm resolution introduces cumulative clock error.
category: plan-completed
status: historical
related_docs:
  - duty-cycling
  - tdma-protocol
  - watchdog-timer
  - tunable-parameters
  - native-test-repair
  - rtc-subsecond-sleep
  - standby-watchdog-coverage
---

# MCU Duty Cycle — Changelog & Implications

## Disposition (audited 2026-08-21)

This was never a plan — it is a **review of one commit**, and it did its job: four of its five
Implications drove concrete follow-on work that has since shipped. Retired to
`Completed_Plans/` as the historical record of why that work happened. **Do not read the
Changelog section below as current behaviour** — most of what it describes has since been
replaced.

Resolution of each Implication:

| # | Implication | Resolution |
|---|---|---|
| 1 | RTCZero whole-second alarm → up to ~1 s uncounted error per cycle, cumulative | **Fixed.** `rtc-subsecond-sleep` Phase 1 replaced MODE2 calendar with MODE0 COUNT32 @1024 Hz. Shipped and baked |
| 2 | `_tdmaClock.reset()` every wake → recurring unslotted AWAKEN handshake | **Fixed.** `rtc-subsecond-sleep` Phase 2 (T6) removed the reset; the session now survives standby. Shipped and baked |
| 3 | Watchdog disabled across standby → no hang recovery for the sleep duration | **Still open.** Carried forward to `standby-watchdog-coverage` |
| 4 | `Hybrid` never enters MCU standby despite its doc comment | **Resolved by decision, not code.** `Hybrid` is deprecated; `DutyCycleMode::Hybrid` and `feather_m0_lora_node_hybrid` remain in the tree but are not a target for further work |
| 5 | Scope check — production env untouched | **Superseded.** No longer true, and no longer the relevant question: `Timed` is the developed path and the whole stack has since been flashed and baked |

Two factual corrections to the Status text below, left in place rather than rewritten so the
record reads as it did at the time:

- "**Unmerged — 1 commit ahead of `master`**" was accurate on 2026-08-04. As of 2026-08-21
  `feature/mcu-duty-cycle` is **13 commits ahead of `master` and still unmerged**, and every
  piece of work since — RTC sub-second sleep, window markers, GPS clock Step 1, `NUM_SLOTS=5`,
  dynamic TX power — sits on it. The branch, not `master`, is what is flashed on hardware.
- The final open question referred to `duty-cycle-test-factory-fix`. That plan has been
  replaced by `native-test-repair`, which covers the same test file plus everything else
  keeping `pio test -e native` red.

## Status (as written 2026-08-04)

Branch `feature/mcu-duty-cycle`, commit `d7ba3c5` (Kyle Krstulich, 2026-08-04). **Unmerged —
1 commit ahead of `master`.** Commit message: *"Duty cycling the MCU now. Timer doesn't work
when MCU sleeps so an RTC class is nessisary to keep TDMA alive. Added more environments and
sensor configs for the different duty cycle options as we move forward with power saving
measures."*

This doc reviews that commit: what changed, and — since the RTC sleep path forces a full TDMA
resync on every wake — what that means for slot timing, airtime, and collision risk once this
ships to multiple field nodes.

## Changelog

### 1. New: MCU can now go into real hardware standby (SAMD21)

- `platform/Samd21RtcSleep.h/.cpp` (new) — wraps `RTCZero`, implements
  `IMcuSleep::sleepFor(requestedMs)`: sets an RTC alarm, disables the watchdog, detaches USB,
  calls `_rtc.standbyMode()` in a loop until the alarm epoch is reached, then re-enables the
  watchdog and USB.
- `platform/ArduinoClock.h/.cpp` — gained `compensateForSleep(elapsedMs)`, which adds a
  cumulative `_sleepOffsetMs` on top of raw `millis()`. This exists because `millis()` (the
  SysTick/TC-driven Arduino timer) **stops advancing during standby** — this is how "app time"
  is made to appear to keep flowing across a hardware sleep.
- New library dependency: `arduino-libraries/RTCZero`.

### 2. `DutyCycleController` reworked from 2 profiles to 4 modes

`power/DutyCycleController.h/.cpp`

- New `DutyCycleMode`: `Continuous`, `SensorTriggered`, `Timed`, `Hybrid`.
- New `timedSleepMs` config field, new `mode()`, `sleeping()`, `timedSleepRemainingMs()`
  accessors.
- Wake logic split into `triggerWakeEnabled()` / `timedWakeEnabled()` /
  `wakeFromSleepIfNeeded()` — `Timed` wakes purely on elapsed time, `Hybrid` wakes on whichever
  of {threshold crossing, timer} comes first, `SensorTriggered` is unchanged (threshold-only,
  same behavior as before this commit).

### 3. `TdmaRadioService`: radio can be told to sleep independent of Rx-gating

`radio/TdmaRadioService.h/.cpp`

- New `setDutySleep(bool)` — when the duty controller is in `CooldownSleeping`/`IdleSleeping`,
  the app now puts the SX1276 in `sleep()` outright, and `update()` short-circuits (skips
  `updateRxPower()`/`drainTxQueue()`) whenever `_dutySleepRequested` is true. This is new and
  separate from the existing base-slot-0 Rx gating (see `radio-rx-gating`).

### 4. `SmartFiresNodeApp`: wired the above together, added `maybeEnterTimedMcuSleep()`

`app/SmartFiresNodeApp.cpp:545`

- Every `update()` tick now computes
  `radioShouldSleep = dutyPhaseSleepsRadio(phase) && !_forceRadioAwake` and calls
  `_radio.setDutySleep(...)`.
- **Only in `DutyCycleMode::Timed`**, once per sleep phase (guarded by `_mcuSleptThisCycle`):
  puts the radio to sleep, floors the remaining sleep time to whole seconds, calls
  `_mcuSleep.sleepFor(standbyMs)`, then unconditionally:
  - `_tdmaClock.reset()` — discards `hasSync`, session anchor, everything.
  - `_syncActive = false`.
  - `_forceRadioAwake = true`, and immediately re-wakes the radio (overriding the phase-based
    sleep gate) so it can hear/send again.
- Back in the main loop, `!hasFreshSync` is now true, so the node falls into the pre-existing
  "waiting for time sync" branch: **re-sends `AWAKEN` every 5 s** (`kAwakenIntervalMs`) until
  the base's deferred direct `TIME_SYNC` reply lands. Every RTC wake now re-runs the full boot
  handshake, not just cold boot.
- Constructor gained an `IMcuSleep&` parameter. Note: there's a leftover duplicate/commented-out
  old constructor declaration in both the header and `.cpp` — dead code, not a functional issue,
  but worth a cleanup pass before merge.

### 5. Build config / `platformio.ini`

- Flag renamed: `SMARTFIRES_DUTY_CYCLE_CONTINUOUS` (0/1) → `SMARTFIRES_DUTY_CYCLE_MODE` (0–3).
  **This makes the existing `duty-cycle-test-factory-fix` plan doubly stale** — it was already
  written against the old flag name/values and references factories that no longer match this
  commit either.
- `feather_m0_lora_node` (**the real deployed node env**) stays on mode `1` = `SensorTriggered`
  — **behaviorally unchanged**: no RTC standby, no forced resync-per-wake.
- `feather_m0_lora_node_debug` (the day-to-day dev/flash env) switched from effectively-continuous
  → mode `2` = `Timed`. **This is almost certainly the env producing the "AWAKEN every wake"
  behavior currently being observed.**
- Two new envs added: `feather_m0_lora_node_timed` (mode 2) and `feather_m0_lora_node_hybrid`
  (mode 3).
- New `Timed` profile constants: `kTimedActiveSampleMs=25000`, `kTimedTimedSleepMs=35000`,
  `kTimedWarmupMs=10000` (~60 s nominal cycle before resync overhead — see Implications).
- New `Hybrid` profile: `kHybridTimedSleepMs = 5 min` as a timer backstop on top of
  threshold-triggered wake.

### 6. Minor, bundled but unrelated to duty-cycle logic

- GPS (`Pa1010dGpsSensor`) `dutyClass` default changed `AlwaysOn` → `DutyCycled`; `powerMode`
  default `PeriodicBackup` → `Backup`; `AdafruitGpsDriver::begin()` now actually drives the
  wake/reset pins (previously commented out).

## Implications

**The "new AWAKEN every wake" behavior isn't drift — it's a full resync forced every cycle —
and RTCZero's whole-second alarm resolution makes the schedule itself imprecise.**

1. **RTCZero's alarm resolution really is whole seconds only** (`MATCH_YYMMDDHHMMSS`), and that
   shows up in two places:
   - `Samd21RtcSleep::sleepFor()` floors the requested sleep to `requestedMs / 1000` seconds
     before setting the alarm.
   - The elapsed time fed back into `ArduinoClock::compensateForSleep()` is
     `(endEpoch - startEpoch) * 1000` — also only ever a whole-second value, because
     `RTCZero::getEpoch()` has no sub-second resolution. Real wall-clock time spent asleep
     almost never lands exactly on a second boundary, so this reported value systematically
     **undercounts** true elapsed time by up to ~1 s per cycle.
   - Because `_sleepOffsetMs` only ever accumulates (`+=`, never corrected), this ~0–1 s
     per-cycle error is **cumulative for the life of the boot session**. On a `Timed` node
     cycling every ~60 s, that's a local-clock error that can grow by up to ~1 s/min if left
     unattended.

2. **TDMA doesn't actually stay "alive" across the sleep — it gets rebuilt from scratch every
   cycle.** `_tdmaClock.reset()` runs unconditionally after every RTC standby, so the
   accumulated clock drift from point 1 never gets to compound into slot phase — each wake
   re-anchors to the base's session time via a fresh AWAKEN → direct-`TIME_SYNC` handshake. So
   slot alignment isn't silently decaying; the design masks the RTC's coarseness by re-syncing
   constantly. The real cost lands elsewhere:
   - **Airtime overhead, every cycle, not just at boot.** `sendAwakenHandshake()` is **not**
     TDMA-slot-gated — it's an immediate `sendToWait()` to the base address regardless of slot.
     Previously this only happened once at cold boot (a rare, one-time contention risk). Now it
     recurs every duty cycle. Nothing in this commit randomizes/jitters wake timing across
     nodes, so multiple `Timed`/`Hybrid` nodes on similar sleep periods are likely to have their
     post-wake AWAKEN retries cluster and collide with each other and with the base's own
     slotted TX.
   - **Hidden dead time added to every cycle, not reflected in the config constants.** After MCU
     wake, the app blocks in the `!hasFreshSync` branch — `_duty.update()` is never called, so
     the duty controller's own `WarmingUp`/`ActiveSampling` clocks don't start ticking until
     resync completes. Real per-cycle cost is
     `sleepMs + resync_latency + warmupMs + activeSampleMs`, not the ~60 s
     (`35 s + 25 s`) the `Timed` profile constants alone imply. Resync latency is bounded (base
     replies within ~one frame period, well inside the 5 s AWAKEN retry) but non-zero and
     variable — expect real cycles noticeably longer than 60 s, with more variance than before.
   - **Base-side cost scales too.** Every wake re-triggers the base's node-assignment/deferred
     direct-`TIME_SYNC` path per node, instead of that happening once at boot.

3. **Watchdog coverage gap during standby.** `Watchdog.disable()` is called before
   `standbyMode()` ("the normal watchdog cannot span a five-minute standby") and only
   re-enabled after wake. If the MCU fails to wake cleanly from standby (bad interrupt state,
   RTC misconfiguration, etc.), there's no watchdog to recover it for the duration of the sleep
   — worth cross-checking against `watchdog-timer`, since this reintroduces the kind of
   unrecoverable-hang window that work was meant to close, scoped specifically to standby.

4. **`Hybrid` mode doesn't actually do what its own doc comment says.**
   `maybeEnterTimedMcuSleep()` gates strictly on `_duty.mode() == DutyCycleMode::Timed`.
   `Hybrid` configures a 5-minute `kHybridTimedSleepMs` backstop and its header comment
   describes sensors sleeping between windows, but the MCU is **never** put into RTC standby in
   `Hybrid` — only the radio gets `setDutySleep(true)`. Today `Hybrid` saves radio power but not
   MCU power, which reads as an oversight worth flagging to Kyle rather than an intentional
   staged rollout (nothing in the commit message suggests it's deliberate).

5. **Scope check:** the production node env (`feather_m0_lora_node`) is untouched — still
   `SensorTriggered`, no RTC standby, no forced per-wake resync. Everything above is live today
   only on `feather_m0_lora_node_debug` (now defaults to `Timed`) and the two new
   `_timed`/`_hybrid` envs. Confirm which env is actually flashed on hardware showing this
   behavior before assuming it affects deployed field nodes.

## Open questions before this merges

- Should a wake from RTC standby try to **resume** the existing TDMA session using the
  sleep-compensated clock (accepting the ~1 s/cycle RTC error) instead of unconditionally
  discarding sync and re-handshaking? Given point 2 above, resync-every-cycle turns AWAKEN from
  a rare boot-time event into a steady recurring one. See `rtc-subsecond-sleep` for a concrete
  plan to fix the underlying clock resolution and then drop the forced resync.
- Should node wake times be jittered/randomized to avoid synchronized AWAKEN storms once
  multiple `Timed`/`Hybrid` nodes are deployed together?
- Is `Hybrid` supposed to enter RTC standby too? If so, `maybeEnterTimedMcuSleep()`'s mode gate
  needs to include it; if not, the profile's doc comment needs correcting.
- `duty-cycle-test-factory-fix` (existing pending plan) already needed updating for the
  `SMARTFIRES_DUTY_CYCLE_CONTINUOUS`→ `kActive*` consolidation; this commit's rename to
  `SMARTFIRES_DUTY_CYCLE_MODE` and the `DutyCycleConfig::make()` signature change (new
  `wakeMode`/`timedSleepMs` params) make it stale a second time over. Worth folding into the
  same fix pass.
