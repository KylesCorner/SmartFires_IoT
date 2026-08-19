---
name: rtc-subsecond-sleep
description: Plan to replace RTCZero's whole-second calendar-mode alarm (used by Samd21RtcSleep for MCU standby) with the SAMD21 RTC's raw COUNT32 tick-counter mode, so sleep-elapsed time is known to ~1 ms instead of ~1 s — precise enough that a node can resume its TDMA session across a sleep instead of discarding sync and re-running the full AWAKEN handshake every wake.
category: plan-pending
status: in-progress — Phase 1 (T0–T4) shipped; Phase 2 (T6) implemented 2026-08-17, awaiting user compile/flash + bake. T8 dropped (Hybrid deprecated).
related_docs:
  - mcu-duty-cycle-changelog
  - duty-cycling
  - tdma-protocol
  - watchdog-timer
  - radio-rx-gating
---

# RTC Sub-Second Sleep

## Background

`platform/Samd21RtcSleep.cpp` puts the node's SAMD21 into standby using `RTCZero`'s
calendar/epoch API — `getEpoch()`, `setAlarmEpoch()`, `MATCH_YYMMDDHHMMSS`. That API only
stores and compares whole seconds, so:

- the requested sleep duration is floored to the nearest second before the alarm is set, and
- the elapsed-time readback used by `ArduinoClock::compensateForSleep()` is also only ever a
  whole-second value (`(endEpoch - startEpoch) * 1000`),

giving up to ~1 s of uncounted error on every sleep/wake cycle. See `mcu-duty-cycle-changelog`
for the full trace, but the practical consequence is that
`SmartFiresNodeApp::maybeEnterTimedMcuSleep()` doesn't trust this error margin against the
20 ms TDMA guard band, so it defensively calls `_tdmaClock.reset()` after every RTC wake and
re-runs the full `AWAKEN` → direct-`TIME_SYNC` boot handshake before resuming telemetry. That
handshake is **not** slot-gated (`sendAwakenHandshake()` sends immediately, outside TDMA
structure), so this turns what was originally a one-time boot-time event into a recurring one —
on every duty cycle, for every duty-cycled node.

The RTC hardware itself doesn't have a 1-second limit — that's specific to RTCZero's calendar
submode (SAMD21 RTC MODE2). The same peripheral also supports **MODE0** (a free-running 32-bit
tick counter with a compare-match alarm), clocked from the same always-on 32.768 kHz oscillator
that already keeps time through STANDBY today. With a suitable prescaler that gives ~1 ms (or
better) resolution instead of ~1000 ms, using hardware already on the board.

Note `TdmaClock::sessionNowMs()` (`radio/TdmaClock.cpp`) is just
`syncSessionMs + (clock.millis() - syncLocalMs)` — it doesn't need an explicit "resume" step to
survive a sleep. It already carries the session forward correctly through any gap in
`clock.millis()`, provided that gap is accurately compensated. The `tdmaClock.reset()` call is
a safety net layered on top by `SmartFiresNodeApp`, not something `TdmaClock` itself requires —
which is why fixing the clock's resolution and then removing that reset call is a coherent,
low-risk pair of changes rather than a redesign of the TDMA session model.

## Goal

Make the RTC-compensated clock accurate enough (comfortably inside the 20 ms guard band) that a
`Timed`/`Hybrid` node can wake from standby and resume its existing TDMA session without
discarding sync — eliminating the recurring, unslotted `AWAKEN` handshake on every duty cycle.

## Plan

### Phase 0 — survey before hand-rolling

Check whether an already-maintained SAMD21 low-power library (e.g. `ArduinoLowPower`, or a
newer release of `RTCZero` itself) already exposes a millisecond-resolution sleep/alarm API
before writing a register-level driver. If one exists and is a clean drop-in, prefer it —
otherwise proceed with a hand-rolled `RTC->MODE0` driver, which is a well-understood, bounded
amount of code (comparable to what `RTCZero` itself already does for MODE2).

### Phase 1 — precise clock, same protocol behavior (data-gathering)

1. Rewrite `Samd21RtcSleep` to configure the RTC in **COUNT32 mode** instead of calendar mode:
   free-running 32-bit tick counter, compare register set to
   `startTicks + requestedMs × ticksPerMs`, prescaler chosen for ~1 ms ticks (e.g. a
   1024 Hz-derived tick, `/32` off the 32.768 kHz source — exact prescaler/GCLK routing to be
   confirmed against the Arduino SAMD core's existing `GCLK_RTC` configuration during
   implementation).
2. Keep the actual CPU sleep-entry mechanics (`SCB` deep-sleep + `__WFI()`, watchdog
   disable/enable, USB detach/attach) as they are today — only the alarm-timing and
   elapsed-readback logic changes.
3. Feed the resulting millisecond-accurate elapsed time into
   `ArduinoClock::compensateForSleep()` (unchanged signature).
4. **Do not remove `_tdmaClock.reset()` yet.** Instead, add debug logging in
   `maybeEnterTimedMcuSleep()`/the post-resync path that records, for every wake:
   predicted slot phase (what `sessionNowMs()`/`currentSlotIndex()` would have been had sync
   been preserved through the sleep) vs. the actual slot phase after the fresh `TIME_SYNC`
   reply. This produces field evidence of real-world error before anything depends on it.
5. Run this on the `feather_m0_lora_node_timed`/`_hybrid` envs for an extended bench/field
   period and confirm the logged phase error consistently stays well inside `guardMs` (20 ms).

### Phase 2 — stop resyncing on sleep-wake

Once Phase 1 data supports it:

1. Remove the unconditional `_tdmaClock.reset()` / `_syncActive = false` / forced
   `sendAwakenHandshake` retry loop specifically from the sleep-wake path in
   `maybeEnterTimedMcuSleep()`. Cold-boot and genuine lost-sync handling (base offline, etc.)
   are untouched — only the sleep-triggered defensive reset goes away.
2. Keep `_forceRadioAwake`/radio-wake-before-sleep-ends as is; it's still needed to have the
   radio listening in time for the node's own next slot, just no longer gated on receiving a
   fresh `TIME_SYNC` first.
3. Re-run the Phase 1 instrumentation to confirm live slot behavior (not just the "predicted
   vs. actual" comparison) matches expectations with sync genuinely preserved.
4. ~~Consider whether `Hybrid` mode should also gain real MCU standby at this point.~~
   **Dropped — `Hybrid` is being deprecated.** `DutyCycleMode::Hybrid` and the
   `feather_m0_lora_node_hybrid` env are left in place but are not a target for further
   work; Phase 2 is validated on `Timed` only.

### Phase 2 addendum — active-window markers on the wire

Added alongside T6 rather than as a separate plan, because the two share a cause: with the
node no longer tearing down its session every duty cycle, the *contents* of an active window
become the meaningful unit of telemetry, and nothing on the wire marked where one started or
ended.

`PktHeader` gains a `flags` byte (4 → 5 bytes, every packet type) carrying
`PKT_FLAG_WINDOW_FIRST`/`PKT_FLAG_WINDOW_LAST`, set on `PKT_BUNDLE` only. Closing a window
also force-flushes the partial bundle left in `PacketHandler`'s accumulator — previously
those samples (10 of every 25 at the current `Timed` constants) sat unsent across the whole
standby and went out mid-way through the following window. See T9/T10 below and
`duty-cycling`'s "Active Windows on the Wire".

## Task breakdown (subagent-ready)

All paths relative to `SmartFires_IoT/platformio/`. Each task is self-contained; execute in
order. **Guardrail (repo `CLAUDE.md`): do not run `pio run`/`pio test`/flash commands — print
the exact command for the user to run instead.**

### Context pack — facts executors need (verified 2026-08-12)

- `include/platform/Samd21RtcSleep.h` — class `Samd21RtcSleep : IMcuSleep`; members
  `ArduinoClock &_clock`, `RTCZero _rtc`, static `onRtcAlarm()`. Interface
  (`include/platform/IMcuSleep.h`): `uint32_t sleepFor(uint32_t requestedMs)`.
- `src/platform/Samd21RtcSleep.cpp` (~89 lines total):
  - `begin()` (line 17): `_rtc.begin(false); _rtc.attachInterrupt(onRtcAlarm);`
  - `sleepFor()` (line 24): floors to whole seconds; `setAlarmEpoch` +
    `MATCH_YYMMDDHHMMSS`; `Watchdog.disable()` (Adafruit_SleepyDog); `Serial.flush()`;
    `USBDevice.detach()`; `do { _rtc.standbyMode(); } while (_rtc.getEpoch() < targetEpoch);`
    `USBDevice.attach()`; elapsed = `(endEpoch - startEpoch) * 1000`;
    `_clock.compensateForSleep(elapsedMs)`; `Watchdog.enable(SystemHealthConfig::Watchdog::kSteadyStateTimeoutMs)`.
- `ArduinoClock` (`src/platform/ArduinoClock.cpp`): `millis()` returns
  `::millis() + _sleepOffsetMs`; `compensateForSleep(elapsedMs)` adds to `_sleepOffsetMs`.
  Signature unchanged by this plan.
- `src/app/SmartFiresNodeApp.cpp`:
  - `maybeEnterTimedMcuSleep()` at line 545. Whole-second flooring of `standbyMs` at
    lines 559–566. Post-wake defensive reset at lines 585–593:
    `_tdmaClock.reset(); _syncActive = false; _forceRadioAwake = true; _radio.setDutySleep(false);`
  - Sync re-establishment: line 178 `if (hasFreshSync && !_syncActive)` → `_syncActive = true`
    (line 187). AWAKEN re-send while unsynced at line 208.
- `TdmaClock` (`include/radio/TdmaClock.h` lines 21–24): `reset()`, `sessionNowMs()`,
  `currentSlotIndex()`. Guard band: `NetworkConfig.h` `kGuardMs = 20` (line 72).
- RTCZero clocking (library `RTCZero.cpp`, in `.pio/libdeps/`): configures GCLK generator 2
  from the 32.768 kHz XOSC32K with division 32 → **1024 Hz into the RTC**, then MODE2
  prescaler DIV1024 → 1 Hz calendar ticks. MODE0 on the same 1024 Hz GCLK with
  `PRESCALER=DIV1` gives **1024 ticks/s ≈ 0.977 ms/tick**.
  Tick math (u32-safe for sleeps ≤ ~1 hr):
  `ticks = (requestedMs * 1024 + 999) / 1000`; `elapsedMs = (elapsedTicks * 1000) / 1024`.
- MODE0 registers: `RTC->MODE0.CTRL` (MODE=0, PRESCALER), `.COUNT`, `.COMP[0].reg`,
  `.INTENSET.bit.CMP0`, `.READREQ` (set RCONT+RREQ for continuous COUNT read sync), wait on
  `.STATUS.bit.SYNCBUSY` after writes. ISR: clear `.INTFLAG.bit.CMP0`.
- Test envs: `platformio.ini` — `[env:feather_m0_lora_node_timed]` line 216
  (`SMARTFIRES_DUTY_CYCLE_MODE=2`), `[env:feather_m0_lora_node_hybrid]` line 249 (`=3`).
  Production env `feather_m0_lora_node` is untouched by this plan.
- Native tests: `test/test_*` (Unity, `pio test -e native`). `Samd21RtcSleep` itself is
  hardware-only (no native suite); logic moved into pure helpers *can* be tested natively.

### Tasks

**T0 — library survey (Phase 0).** Check whether `RTCZero` (current release) or
`ArduinoLowPower` exposes a ms-resolution standby alarm on SAMD21. Look at
`.pio/libdeps/feather_m0_lora_node_timed/RTCZero/src/RTCZero.{h,cpp}` and the libraries'
GitHub docs. Deliverable: short note appended to this doc's Open Questions — "drop-in exists:
use it (name/API)" or "proceed with MODE0 driver". Expected outcome: proceed (RTCZero is
calendar-only; ArduinoLowPower's `sleep(ms)` uses RTCZero epoch alarms → same 1 s floor).

**T1 — tick-math helpers + native tests.** Add a small header
`include/platform/Samd21RtcTicks.h` with `constexpr`/inline pure functions:
`msToTicks(uint32_t ms)` and `ticksToMs(uint32_t ticks)` using the 1024 Hz math above, plus
32-bit-wrap-safe elapsed-tick delta `tickDelta(start, end)` (`end - start` in u32 is already
wrap-safe; make that explicit). Add `test/test_rtc_ticks/` Unity suite covering: round-trip
error ≤ 1 ms for 1 ms–3 600 000 ms, wraparound delta, `msToTicks(0)==0`. No hardware deps in
the header. Verify: user runs `pio test -e native`.

**T2 — MODE0 rewrite of `Samd21RtcSleep` (Phase 1, steps 1–3).** Rewrite
`src/platform/Samd21RtcSleep.cpp` (+ header) to:

1. `begin()`: keep `_rtc.begin(false)` so RTCZero still owns GCLK setup and the RTC IRQ vector
   is enabled, then reconfigure `RTC->MODE0` per the register notes above (disable, set
   MODE=0/PRESCALER=DIV1, enable CMP0 interrupt, re-enable, sync-wait). Keep NVIC/interrupt
   attach via RTCZero if compatible; if `_rtc.attachInterrupt` assumes MODE2, install a raw
   `RTC_Handler` that clears CMP0 (RTCZero's handler only touches MODE2 ALARM flags — check and
   note which path was taken).
2. `sleepFor(requestedMs)`: read `COUNT` (start), `COMP[0] = start + msToTicks(requestedMs)`,
   then the **unchanged** sequence: `Watchdog.disable()`, `Serial.flush()`,
   `USBDevice.detach()`, `do { standby } while (tickDelta(start, COUNT) < targetTicks)`,
   `USBDevice.attach()`, disable CMP0. Return `ticksToMs(tickDelta(start, endCount))` and feed
   it to `_clock.compensateForSleep()` as today. Keep both LOG_INFO lines, s/epoch/ticks/.
3. Use helpers from T1; keep CPU-standby mechanics (`_rtc.standbyMode()` or equivalent
   `SCB->SCR |= SLEEPDEEP; __WFI()`) byte-for-byte in behavior.

Verify: user runs `pio run -e feather_m0_lora_node_timed` (compile only).

**T3 — drop the sub-second floor in the caller.** In
`src/app/SmartFiresNodeApp.cpp:559-566`, replace the whole-second floor with a minimum-sleep
threshold: `if (remainingMs < kMinStandbyMs) return false;` then pass `remainingMs` directly
to `_mcuSleep.sleepFor()`. Add `kMinStandbyMs` (suggest 250 ms) near the top of the file or in
`SensingConfig.h` with a one-line comment. Update the stale "RTCZero uses whole-second alarms"
comment. Verify: compile as T2.

**T4 — phase-error instrumentation (Phase 1, step 4).** In `SmartFiresNodeApp`:

1. Just **before** `_tdmaClock.reset()` (line 585), if sync was active, capture
   `_predictedSessionMs = _tdmaClock.sessionNowMs()` and `_predictedValid = true` (new private
   members; `sessionNowMs()` already includes the just-applied sleep compensation).
2. In the resync branch (line 178 block, where `_syncActive` flips true), if `_predictedValid`:
   `LOG_INFO("sleep", "wake_phase_err predicted_ms=%lu actual_ms=%lu err_ms=%ld", …)` using
   `_tdmaClock.sessionNowMs()` as actual; clear `_predictedValid`. Signed error via
   `(int32_t)(actual - predicted)`.
3. Do **not** touch the reset itself — Phase 1 keeps the defensive resync.

Verify: compile; field logs later show `err_ms` distribution vs the 20 ms guard.

**T5 — bake period (no code).** User flashes `feather_m0_lora_node_timed` (and `_hybrid` if
desired) and collects `wake_phase_err` lines over ≥ 100 wake cycles (grep `@SFDBG` sleep tag,
see `documentation/User_Reference/DEBUG_FILTER.md`). Gate for Phase 2: |err_ms| consistently
< 10 ms (half the guard band).

**T6 — remove sleep-wake resync (Phase 2, step 1–2).** In `maybeEnterTimedMcuSleep()`
(lines 585–593): delete `_tdmaClock.reset()` and `_syncActive = false` from the sleep-wake
path only (cold-boot/lost-sync paths at lines 196–208 and 505–506 stay). Keep
`_forceRadioAwake = true; _radio.setDutySleep(false);` so the radio is listening before the
node's next slot. Repoint the T4 instrumentation: predicted-vs-actual now compares against the
next naturally received TIME_SYNC instead of a forced one (same log line works — leave it in).
Verify: compile; then user re-runs the T5 bake and confirms nodes hit their slots (sniffer:
`smartfires-edge web --sniffer-port …`, slot/jitter stats in `sniffer_service.py`).

**T7 — power measurement (after T6).** No firmware change. User benches baseline vs post-T6
current using `feather_m0_sensor_probe`-style methodology + `util/scope_current_log.py`
(same approach as `[[project_lora_rx_gating]]`). Record results in
`MCU_DUTY_CYCLE_CHANGELOG.md`.

**T8 — Hybrid standby (Phase 2, step 4, optional). DROPPED.** `Hybrid` is being deprecated,
so letting `DutyCycleMode::Hybrid` call `maybeEnterTimedMcuSleep()` is no longer a goal. The
mode and its env remain compilable but out of scope.

**T9 — `PktHeader::flags` + window markers.** Add a `flags` byte to `PktHeader` and
`PKT_FLAG_WINDOW_FIRST`/`PKT_FLAG_WINDOW_LAST`; thread an optional `flags` argument through
every encoder; teach `PacketHandler` `beginWindow()`/`flushWindow()`; drive both from
`SmartFiresNodeApp::updateWindowMarkers()` off the `ActiveSampling` phase edges. Mirror the
header change in `edge/edge-receiver`'s `packet.py`, `uart_receiver.py`, `sniffer_service.py`
and add `pkt_flags`/`window_first`/`window_last` CSV columns.

**T10 — drain TX queue before standby.** `maybeEnterTimedMcuSleep()` waits for
`TdmaRadioService::queuedCount() == 0` (cap `SensingConfig::DutyCycle::kMaxTxDrainBeforeStandbyMs`,
5 s) before entering standby, so the window-flush bundle actually gets its TDMA slot instead
of being parked in the queue for the whole sleep.

## Power consumption impact

Short answer: the RTC-mode change itself is roughly power-neutral; the actual power win is in
Phase 2, from removing the recurring handshake — not from anything about COUNT32 vs. calendar
mode.

- **RTC peripheral power is not meaningfully affected by MODE0 vs. MODE2.** Both submodes run
  off the same always-on 32.768 kHz source, which is already kept enabled through STANDBY today
  specifically so `RTCZero` can wake the chip — that's the dominant (and already-paid) power
  cost, on the order of ~1–2 µA per the SAMD21 datasheet's "standby with RTC running" figures.
  Ticking the internal counter at ~1 kHz instead of 1 Hz adds negligible switching current
  (nanoamp-scale) against that baseline — not something a bench measurement will likely resolve
  as different from noise.
- **Phase 1 alone should show no measurable power change** versus current behavior — sleep
  duration, active-window duration, and the resync handshake are all unchanged; only the
  precision of a background compensation value changes.
- **Phase 2 is where the savings are.** Today, every wake forces: radio wake, an `AWAKEN` send
  (with link-ACK wait), potential 5 s-spaced retries until a direct `TIME_SYNC` reply lands, then
  `warmupMs` before the duty controller even starts its active-sampling clock. All of that is
  radio-active and/or full-CPU-active time — orders of magnitude higher current draw than deep
  standby. Removing it shortens the active portion of every cycle back down to roughly
  `warmupMs + activeSampleMs`, recovering whatever the resync/retry window was costing on top.
  That recovered time also does not currently exist in the `Timed`/`Hybrid` config constants —
  see `mcu-duty-cycle-changelog`'s cycle-length estimate — so eliminating it also makes real
  cycle length match the configured duty-cycle profile again, not just save power.
- **Minor secondary effect:** today's flooring behavior in `Samd21RtcSleep::sleepFor()` combined
  with the `do { standbyMode(); } while (getEpoch() < targetEpoch)` loop means actual standby
  duration is always *at least* the requested whole seconds, sometimes up to ~1 s more, with the
  watchdog disabled for that entire (slightly unpredictable) window. Millisecond-accurate timing
  tightens the watchdog-disabled window to match the intended sleep duration — a small
  correctness improvement alongside the power discussion, not itself a significant power number.

Net expectation: don't bench-test Phase 1 expecting a power win — instrument it for correctness
(clock error), and measure power before/after **Phase 2**, where the removed handshake should
show up clearly.

## Verification

- Bench current-draw comparison using the existing `feather_m0_sensor_probe` env and
  `util/scope_current_log.py` tooling (already used for prior power work — see
  `[[project_lora_rx_gating]]`'s ~56% radio-current projection for the same measurement
  approach): baseline (current `feather_m0_lora_node_timed` behavior) vs. post-Phase-2.
- Field/bench log analysis of the Phase 1 "predicted vs. actual slot phase" instrumentation
  across many wake cycles, across more than one node if possible, before trusting Phase 2.
- Confirm RTC MODE0 compare-match interrupt reliably wakes the chip from STANDBY on real
  hardware — some SAMD21 revisions have RTC/standby-wake errata worth checking against the
  datasheet errata sheet before relying on this in the field.
- Re-run watchdog-disabled-window reasoning against `watchdog-timer` once actual sleep duration
  is tighter, to confirm no new gap opened.

## Execution log (2026-08-12)

- **T0 done** — no drop-in library; MODE0 driver confirmed as the path (see survey note under
  Open Questions). RTCZero's `RTC_Handler` turned out to be MODE0-compatible (INTFLAG bit 0
  aliasing), so no raw handler was installed — RTCZero stays linked and owns the vector.
- **T1 done** — `include/platform/Samd21RtcTicks.h` + `test/test_rtc_ticks/`. The ceil/floor
  pairing makes the round trip *exact* (not just ≤ 1 ms) for all sleeps up to ~48.5 days
  (the u32 tick-representability bound); tests assert this. Math additionally verified via
  compile-time `static_assert` harness during implementation.
- **T2 done** — `Samd21RtcSleep` rewritten to MODE0 COUNT32 @1024 Hz, CMP0 alarm. `begin()`
  keeps `_rtc.begin(false)` for GCLK/NVIC, then SWRSTs the peripheral into MODE0. COUNT reads
  use manual `RREQ` + `SYNCBUSY` wait per read (not `RCONT`), sidestepping the
  continuous-read-after-standby-wake errata; the ~1–2-tick read-sync latency is common to the
  start/end reads and cancels in the delta. Standby entry/exit sequence (watchdog, USB
  detach/attach, re-standby loop) unchanged.
- **T3 done** — whole-second floor replaced by `SensingConfig::DutyCycle::kMinMcuStandbyMs`
  (250 ms); full `remainingMs` now goes to `sleepFor()`.
- **T4 done, with one deviation from the recipe above:** storing absolute
  `sessionNowMs()` at wake and comparing at resync would fold the awake gap (AWAKEN retry
  wait, possibly seconds) into `err_ms`. Instead the app stores the session-clock **offset**
  (`sessionNowMs() - clock.millis()`, i.e. the sleep-compensated projection of the old sync)
  and projects it forward at resync time: `predicted = offset + millis()` at the instant the
  fresh sync lands. `err_ms` therefore isolates RTC compensation error as intended. A pending
  prediction is also invalidated on `consumeSessionChanged()` — a new session id means a new
  clock origin and the comparison would be garbage.
- **Next:** user compiles + runs native tests, flashes `_timed`, then T5 bake
  (≥100 wakes, gate |err_ms| < 10 ms).

## Execution log (2026-08-17) — Phase 2

- **T6 done.** `maybeEnterTimedMcuSleep()` no longer calls `_tdmaClock.reset()` /
  `_syncActive = false` after standby, so the session survives the sleep and the recurring
  unslotted `AWAKEN` → `TIME_SYNC` handshake is gone. Cold boot and genuine lost/stale sync
  are untouched — `update()`'s `hasFreshSync` check still drives the AWAKEN retry loop.
  `_forceRadioAwake` / `setDutySleep(false)` are kept as planned.
  - **One thing the plan didn't anticipate:** `_forceRadioAwake` was only ever cleared inside
    the `hasFreshSync && !_syncActive` branch — i.e. by the very resync T6 deletes. Left as
    written, the radio would have stayed force-awake forever after the first standby, quietly
    cancelling `[[project_lora_rx_gating]]`. It is now cleared when the duty controller leaves
    its sleeping phase (`!_duty.sleeping()`), which is where the override stops being needed.
  - Instrumentation repointed as the plan specified, but it could not stay on the
    sync-acquired edge — that edge no longer fires. `TdmaClock` gained `syncLocalMs()`, and
    `logWakePhaseErrorOnNextSync()` fires `wake_phase_err` when that timestamp moves, i.e. on
    the next naturally received `TIME_SYNC`. Same log line, now with `guard_ms` alongside.
- **T9 done.** `PktHeader` is 5 bytes; `PKT_FLAG_WINDOW_FIRST`/`PKT_FLAG_WINDOW_LAST` set on
  bundles only. `flushWindow()` handles the case where a completed bundle hasn't been taken
  yet by stamping `WINDOW_LAST` into the existing frame **and recomputing its crc8** rather
  than overwriting the buffer. Seven native tests added to `test/test_packet_handler/`.
- **T10 done.** `kMaxTxDrainBeforeStandbyMs = 5000` in `SensingConfig::DutyCycle`.
- **Also fixed:** `test/test_rtc_ticks/test_main.cpp`'s native `main()` called `delay(2000)`
  without ever including `<Arduino.h>`. The repo *does* ship a native shim
  (`test/support/Arduino.h`, on the include path via `-Itest/support`) that defines `delay`
  as a no-op — the other suites pick it up transitively through `FakeClock.h` — but this
  suite includes only `Samd21RtcTicks.h`, so `delay` was undeclared and the suite could not
  build, failing `pio test -e native` as a whole. Removed. (`test/test_config` still fails
  to compile for the separate reason tracked in `[[duty-cycle-test-factory-fix]]`.)
- **Wire compatibility:** the header change is a hard break. Nodes, the base, and the Jetson
  package must be updated together — a mixed set will CRC-fail every packet. The legacy
  9-byte `AWAKEN` decode still works (it is parsed against the old 4-byte header) but only
  makes an old node's handshake visible, not its telemetry.

> **Superseded in part (2026-08-19).** T9's `PKT_FLAG_WINDOW_FIRST`/`PKT_FLAG_WINDOW_LAST` and
> T11's `asleep`-on-fresh-`WINDOW_LAST` rule have been replaced by dedicated
> `PKT_WINDOW_BEGIN`/`PKT_WINDOW_END` frames — see `window-marker-packets`. The T11 bake note
> below predicting "at most one `retx` bundle per window during warmup" was correct, and that
> per-cycle duplicate is what the new plan removes: the replay existed only to prompt the ack
> the base was deferring. `notifyMcuStandby()`, `PKT_FLAG_RETX` and the defer-don't-drop
> behaviour all survive unchanged; what changed is what carries the sleep/wake signal.

### T11 (added 2026-08-17) — surviving the standby, both sides of the ack loop

Raised by the user after T6/T9 landed: *what happens to samples and packets that haven't
been sent when the node sleeps, and the base needs to stop sending ACK_SUMMARY to a node
that has gone down.* Investigated, then implemented as designed option (b) — defer the
acknowledgement rather than drop it, so the window-close bundle stays recoverable.

- **Nothing is lost to memory.** SAMD21 standby retains SRAM; `Samd21RtcSleep::sleepFor()`
  calls `_rtc.standbyMode()`, not a reset. `TdmaTxQueue`, `PacketHandler`'s accumulator and
  `TdmaRadioService::_pending[]` are all file-scope globals in `main.cpp` and come back
  byte-identical. No ring-buffer persistence is needed. (`flushTelemetryBuffers()` is the
  only thing that would destroy the queue and it is only reachable from `cmd_reset_soft`.)
- **What *was* being lost, deterministically:** pending-window entries age against
  `sessionNowMs()`, which after T6 now runs through standby. `kTimedSleepMs` (35 s) exceeds
  `kReliabilityMaxAgeMs` (30 s), so every sent-but-unacked bundle was discarded as
  `max_age` on the first post-wake drain, with no retransmit — not a race, it fired every
  cycle. It landed hardest on the `WINDOW_LAST` bundle, whose ack can only be sent in a
  slot 0 that falls after standby has already begun, making it structurally unackable.
  Fixed by `TdmaRadioService::notifyMcuStandby(elapsedMs)`, which slides
  `firstSentMs`/`lastSentMs` (and `_lastAckSummarySessionMs`, which
  `requireAckSummaryBeforeFirstRetry` compares against them) forward by the measured sleep.
  Entries then become eligible one `retryWaitMs` (8 s) into the wake — inside `warmupMs`
  (10 s), when no fresh telemetry is competing for the node's slot.
- **New wire bit `PKT_FLAG_RETX = 0x04`.** `pickRetransmitCandidate()` ORs it into the
  outgoing *copy* and recomputes the crc8; the stored payload is untouched so repeated
  attempts are byte-identical. Needed because a replayed `WINDOW_LAST` means the opposite
  of a fresh one — the node is awake and re-asking. Also flags replayed rows to the Jetson
  (`retx` CSV column; they duplicate an earlier `node_id`+`seq`, they are not new samples).
- **Base defers, does not drop.** `AckTracker` gained `asleep` (set on a fresh, non-`RETX`
  `WINDOW_LAST`; cleared by any later frame), `forceResend` (a `RETX` frame is proof the
  last ack never landed, so it bypasses the `unchangedFromLastSent` suppression for one
  send) and `lastHeard*`. `sendPendingAckSummary()` skips gated trackers **with `dirty`
  left set**, so the ack goes out on the first slot 0 after the node is heard again.
  Motivation beyond tidiness: `sendAckSummary()` uses blocking `sendToWait()`
  (≈1 s, longer than the base's own 900 ms slot 0), so the base was spending ~3 slot-0
  windows per node per duty cycle blocked against a switched-off radio, delaying
  `TIME_SYNC` and commands for everyone else. `kAckSummaryNodeSilenceMs`
  (2 frame periods) covers a `WINDOW_LAST` that was itself lost.
- **Tests:** new `test/test_tdma_radio_service/` with `FakeTdmaRadioDriver` — 4 cases
  covering the without-notify loss, the with-notify survival, the `RETX` stamp (flags +
  recomputed crc8 + preserved window bits) and byte-identical repeat attempts. Required
  adding `radio/TdmaRadioService.cpp` and `radio/TdmaTxQueue.cpp` to the `native` env's
  `build_src_filter`; they were not previously compiled for tests at all.
- **Known gap, not fixed:** queued `CMD_CALIBRATE`/`CMD_RESET` use the same blocking send
  to the same deaf node, and `kMaxPendingCommandSendAttempts` (3, ≈11 s) expires well
  inside a 35 s standby, so operator commands to a sleeping `Timed` node are dropped rather
  than deferred. Gating them needs a deferral deadline so a dead node can't hold a command
  slot forever — separate design.

- **Next:** user compiles, runs `pio test -e native`, flashes base + node, reinstalls the edge
  package, then bakes: confirm `wake_phase_err` stays inside the guard band with sync genuinely
  preserved, confirm nodes hit their slots on the sniffer, and confirm every window shows a
  `window_first` and (where samples don't land on a bundle boundary) a `window_last` in the CSV.
  For T11 specifically: expect `pending_sleep_shift` on each wake, at most one `retx` bundle
  per window during warmup, `ack_summary_defer` on the base while a node is down, and
  `drop_pending reason=max_age` to stop appearing on every cycle.

## Open questions

### T0 survey result (2026-08-12): proceed with MODE0 driver

No drop-in exists. Checked upstream sources (library not yet in `.pio/libdeps/` — the
`_timed` env hasn't been built on this machine):

- **RTCZero (master):** MODE2 calendar only; alarms via `MATCH_YYMMDDHHMMSS` at 1 s
  resolution. No MODE0/COUNT32 or sub-second API.
- **ArduinoLowPower (master, SAMD):** `sleep(ms)` → `setAlarmIn(ms)` →
  `rtc.setAlarmEpoch(now + millis/1000)` — same 1 s floor, just wrapped.

Two implementation facts confirmed from RTCZero source that shape T2:

1. **Clocking:** `configureClock()` sets `GCLK_GENDIV_DIV(4)` **with `GCLK_GENCTRL_DIVSEL`
   set**, i.e. division = 2^(4+1) = 32 → **1024 Hz into the RTC** from XOSC32K, as assumed.
2. **`RTC_Handler` is reusable in MODE0:** RTCZero.cpp defines `RTC_Handler` (so a second
   definition would be a duplicate-symbol link error while RTCZero stays linked), but its body
   calls the attached callback and then writes `RTC->MODE2.INTFLAG.reg =
   RTC_MODE2_INTFLAG_ALARM0` — INTFLAG is the same register in every RTC submode and
   MODE2 `ALARM0` is bit 0, the same bit as MODE0 `CMP0`. So RTCZero's handler both fires the
   callback and clears CMP0 correctly in MODE0. T2 therefore keeps
   `_rtc.attachInterrupt(onRtcAlarm)` and does **not** install a raw handler.

- Exact prescaler/`GCLK_RTC` routing to use for the COUNT32 tick rate — needs confirming against
  what the Arduino SAMD core already configures for `GCLK_RTC` (RTCZero currently assumes /1024
  to reach 1 Hz calendar ticks; COUNT32 mode can use a different prescaler on the same clock
  input).
- Whether to keep using `RTCZero` for CPU sleep-entry mechanics while driving `RTC->MODE0`
  registers directly for the alarm (lower risk, reuses validated standby-entry code), or drop
  the library dependency entirely in favor of a self-contained driver. Leaning toward the
  former unless it turns out `RTCZero`'s init assumes calendar mode in a way that conflicts.
- Rollout: land Phase 1 behind the existing `feather_m0_lora_node_timed`/`_hybrid` envs (already
  isolated from the production `feather_m0_lora_node` env) so this can bake before it's anywhere
  near field nodes, consistent with how `mcu-duty-cycle-changelog` scoped the original commit.
