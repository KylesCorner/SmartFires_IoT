---
name: standby-watchdog-coverage
description: The node's hardware watchdog is disabled for the whole of every MCU standby, leaving roughly 47% of wall-clock time with no hang recovery on a Timed node. Scopes the options for closing that window, and re-examines the stale five-minute justification the disable was originally written against.
category: plan-pending
status: draft — 2026-08-21, not implemented
related_docs:
  - watchdog-timer
  - duty-cycling
  - mcu-duty-cycle-changelog
  - rtc-subsecond-sleep
  - reset-reason-diagnostics
---

# Standby Watchdog Coverage

## The gap

`Samd21RtcSleep::sleepFor()` (`src/platform/Samd21RtcSleep.cpp:41-42, 66-68`) disables the
watchdog before entering standby and re-enables it on wake:

```cpp
// The normal watchdog cannot span a five-minute standby.
Watchdog.disable();
...
do { _rtc.standby(); } while (tickDelta(startTicks, _rtc.count()) < targetTicks);
...
Watchdog.enable(SystemHealthConfig::Watchdog::kSteadyStateTimeoutMs);
```

If the MCU fails to wake cleanly — a bad interrupt state, an RTC misconfiguration, a standby
entry that never satisfies the loop's exit condition — nothing recovers it. `WATCHDOG_TIMER`
(completed) exists precisely to make unrecoverable hangs recoverable, and MCU standby
reintroduces exactly that class of window, scoped to the sleep.

This was flagged as Implication 3 of `mcu-duty-cycle-changelog` and is the one item from that
review that has not been resolved.

## Sizing it — the exposure is much larger than "occasionally"

On the current `Timed` profile (`SensingConfig.h:150-201`) the cycle is 10 s warmup + 30 s
active window + **35 s standby**, on a fixed 75 s wake-to-wake period. So the node spends
roughly **47% of wall-clock time with no watchdog**, every cycle, indefinitely — not a rare
edge case. `kSteadyStateTimeoutMs` is 8000 ms, so the covered portion is watched roughly four
times more tightly than the uncovered portion is watched at all.

Worth weighing against evidence that the hangs are real: the 2026-07-14 overnight run recorded
15 WDT-recovered reboots in 16.5 h. Those were all recovered *because the watchdog was
running*. The relevant question is whether the standby path can hang at all, not whether this
firmware hangs.

## The stale premise

The comment justifies the disable against "a five-minute standby". That number came from
`kHybridTimedSleepMs`, the `Hybrid` profile's 5-minute backstop — and `Hybrid` **never enters
MCU standby** (`maybeEnterTimedMcuSleep()` gates strictly on `DutyCycleMode::Timed`), and is
now deprecated besides. The real standby this code has to survive is 35 s, floored at
`kTimedMinStandbyMs` = 5 s.

That does not by itself make the disable wrong — 35 s is still well beyond what the SAMD21
watchdog can be programmed to in its normal configuration — but it changes the shape of the
problem from "impossible by an order of magnitude" to "a factor of two or three", which is
the range where a periodic-pet scheme becomes worth costing out. **Fix the comment regardless
of which option below is chosen**; it currently points a reader at the wrong constraint.

## Options

Roughly in order of increasing cost. None is obviously correct yet — this needs a decision,
and options 2 and 3 need hardware facts confirmed first.

### 1. Accept the gap, document it, bound the sleep

Do nothing to the mechanism; correct the comment to state the real standby duration and the
accepted risk, and rely on `kTimedCyclePeriodMs`/`kTimedMinStandbyMs` to keep the exposure
bounded. Cheapest, and defensible **if** a failed standby wake turns out to be a failure mode
nobody has ever observed. Weak point: a node stuck in standby is silent, and a silent node is
indistinguishable from one out of radio range — so this failure would not be noticed as a
distinct thing. Before choosing this, check whether the field data can even tell the two
apart.

### 2. Keep the watchdog running through standby

The SAMD21 WDT can be clocked from a generator that keeps running in standby, which would let
it fire during the sleep rather than be disabled across it. The obstacle is the achievable
timeout: the WDT period field caps out well below 35 s at any usable clock, so this cannot
simply span the sleep — it needs the sleep broken into WDT-sized segments with a pet in
between, i.e. option 3.

**Confirm before scoping further:** the maximum programmable WDT period on SAMD21 at the
available clock rates, and whether `Adafruit_SleepyDog` exposes it (the library caps SAMD21
timeouts, and the cap is believed to be ~16 s — verify against the library source rather than
trusting this note). If the achievable period exceeds the standby, this collapses into a much
simpler change than option 3.

### 3. Segment the standby and pet the watchdog between segments

Replace the single `_rtc.standby()` sleep with N shorter RTC-alarm sleeps, petting the
watchdog on each wake. Closes the gap completely without abandoning `Adafruit_SleepyDog`.

The already-present `do { _rtc.standby(); } while (...)` re-entry loop is most of the
machinery — it exists to survive spurious wakes, and a scheduled intermediate wake is just a
deliberate one.

Cost is a power question, and it is the one that decides this option: each extra wake pays a
CPU wake, an oscillator settle and a `Watchdog.reset()`. At a ~16 s WDT period a 35 s standby
needs two intermediate wakes per cycle. Measure against `POWER_MEASURMENTS.md` before
committing — if two wakes per 75 s cycle are lost in the noise next to the radio and sensor
draw, this is the right answer.

Note the wake must **not** disturb the RTC compare already armed for the real wake, and must
not touch USB (`USBDevice.attach()`/`detach()` bracket the whole sleep today, deliberately).

### 4. WDT early-warning interrupt as a standby liveness check

Out of scope here, but noted because it shares machinery with `reset-reason-diagnostics`
Phase 3 (deferred), which also wants direct WDT register control instead of
`Adafruit_SleepyDog`. If Phase 3 is ever picked up, revisit this together — doing both
against a hand-rolled WDT driver is cheaper than doing either twice.

## Verification

Whichever option is chosen, the test is an **induced standby hang**: a build-flag-gated debug
hook that enters standby with the wake alarm deliberately never armed, then confirms the node
reboots rather than sitting dark. This is the same shape as the induced-hang test
`reset-reason-diagnostics` still owes, and it should reuse that harness.

With `reset-reason-diagnostics` Phase 1/2 already shipped, the recovery is also *attributable*
— the post-reboot `AWAKEN` carries `reset_cause` and `hang_zone`, so a standby-hang recovery
can be told apart from an ordinary WDT reboot in the dashboard's Node Reboot Events table. Add
a dedicated zone (`ZONE_MCU_STANDBY`) as part of this work; without one the recovery reports
whatever zone was marked before the sleep, which would be misleading.

For option 3, additionally confirm from the bake that `planned_sleep_ms` in `WINDOW_END` and
the measured wake-to-wake period are unchanged — segmenting the sleep must not lengthen the
cycle.

## Open questions

- Can the field data currently distinguish "node stuck in standby" from "node out of range or
  dead"? This decides whether option 1 is defensible or merely invisible.
- Has a failed standby wake ever actually been observed? Nothing in the 2026-07-14 run report
  attributes a reboot to one, but that run predates the RTC standby path being on the
  developed configuration.
- What is the real measured cost of an extra standby wake on this hardware? Option 3 lives or
  dies on this number and it is not in `POWER_MEASURMENTS.md` today.
