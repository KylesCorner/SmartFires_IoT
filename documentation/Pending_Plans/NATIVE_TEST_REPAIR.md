---
name: native-test-repair
description: Consolidated plan to get `pio test -e native` building and green again — repairs test_config's calls to removed DutyCycleConfig factories, resolves six pre-existing test_duty_cycle_controller assertion failures, and removes the stale test/support/Arduino.cpp shim that conflicts with Arduino.h.
category: plan-pending
status: draft — 2026-08-21, not implemented
related_docs:
  - duty-cycling
  - tunable-parameters
  - window-marker-packets
  - rtc-subsecond-sleep
  - mcu-duty-cycle-changelog
---

# Native Test Repair

## Why this doc exists

`pio test -e native` does not currently pass, and the reasons were scattered across three
separate documents — a plan of its own (`DUTY_CYCLE_TEST_FACTORY_FIX`, now replaced by this
one), an execution log (`rtc-subsecond-sleep`), and another plan's open items
(`window-marker-packets`). No single place said "here is why the native suite is red."
This is that place.

The failures are independent of each other and can land in any order. None of them touch
firmware behaviour — this is entirely test-side work, with one config-constant assertion as
the only thing that could plausibly catch a real regression.

**Guardrail (repo `CLAUDE.md`): do not run `pio run`/`pio test`/flash commands. Print the
exact command for the user to run.**

---

## Failure 1 — `test_config` calls factories that no longer exist

**Blocks compilation of the whole `native` env**, so it masks everything else.

`test/test_config/test_main.cpp:116` and `:126` call:

```cpp
DutyCycleConfig cfg = DutyCycleConfig::dutyCycleCfgContinuous();   // gone
DutyCycleConfig cfg = DutyCycleConfig::dutyCycleCfg();             // gone
```

`include/power/DutyCycleController.h:77` defines exactly one factory today,
`DutyCycleConfig::make(...)`. The pair was removed by an earlier consolidation and this file
was never updated.

### The API has moved twice since this was first written down

The superseded `DUTY_CYCLE_TEST_FACTORY_FIX` plan proposed fixing this against a
`SMARTFIRES_DUTY_CYCLE_CONTINUOUS` (0/1) flag and a 9-argument `make()`. **Both are stale.**
Verified at audit:

- The build flag is now `SMARTFIRES_DUTY_CYCLE_MODE`, values `0=Continuous`,
  `1=SensorTriggered`, `2=Timed`, `3=Hybrid` (`platformio.ini:184, 223, 259, 292`).
- `make()` now takes **12** parameters led by `DutyCycleMode wakeMode_`, not `bool enabled_`
  (`DutyCycleController.h:77-90`). The added fields are `cyclePeriodMs_`, `minStandbyMs_`
  and `activeOverrunMaxMs_`, all from the fixed-period Timed window work.
- The resolved constant set is `SensingConfig::DutyCycle::kActive*`
  (`SensingConfig.h:275-300`), which now includes `kActiveMode`, `kActiveCyclePeriodMs`,
  `kActiveMinStandbyMs` and `kActiveActiveOverrunMaxMs`.

There is also dead code to clear: a commented-out copy of the **old** `make()` signature
still sits at `DutyCycleController.h:130-138`. Delete it — it is the thing that will send the
next reader down the wrong path.

### Fix

Replace the two broken functions with one,
`test_duty_cycle_active_profile_matches_sensing_config`, that mirrors what `main.cpp`
actually does: call `make()` with the full current `kActive*` set and assert each field
round-trips. Update the `RUN_TEST` list (`test_main.cpp:218-219` — remove both, add one).

**Caveat to write into the test as a comment.** `[env:native]` never defines
`SMARTFIRES_DUTY_CYCLE_MODE`, so `SensingConfig.h`'s `#ifndef` guard picks the default and
`kActive*` only ever resolves to **one** branch under native. This test therefore cannot
catch drift in the branches that are not compiled. Say so explicitly so a future reader does
not assume otherwise.

To retain some coverage of the uncompiled branches, add assertions that pin the raw
per-profile constants directly rather than whichever one `kActive*` aliases — e.g. that
`kTimedActiveSampleMs` still equals `kTimedBundlesPerWindow * kSamplesPerBundle *
kTimedSamplePeriodMs`, and that `kTimedCyclePeriodMs` still leaves at least
`kMinMcuStandbyMs` of standby. Note both of those already have `static_assert`s in
`SensingConfig.h:218-223`, so prefer assertions that add something those don't — the point
of a tripwire test is to catch drift the compiler won't.

---

## Failure 2 — six pre-existing `test_duty_cycle_controller` assertion failures

Inherited from `window-marker-packets`, which found them and deliberately did not guess at
them. This suite **had not been running at all** — every `DutyCycleController` call site
passed 5 args to a 6-arg constructor, so it could not compile. Fixing that (a new
`FakeAnalogReader` behind a real `BatteryMonitor`) is what exposed the six failures. They are
in tests untouched by that work, concerning behaviour it did not change:

| Failing assertion | What it asserts |
|---|---|
| `begin()` call counts | Expected number of sensor `begin()`/`sleep()` calls at startup |
| two `trigger.serviceCount` tests | Assert the trigger sensor is serviced — but it is not in the `sensors[]` array that `serviceAllSensors()` walks |
| `test_idle_sleeping_does_not_wake_before_min_sleep_...` | Arithmetic assumes `sleepElapsedMs()` restarts at `IdleSleeping`, whereas `_sleepStartMs` deliberately spans both sleep phases |

**These need a decision, not a fix.** Each one encodes an assumption that may have
intentionally changed, and the honest resolution differs per case: update the assertion where
the behaviour was deliberately changed, fix the controller where it was not. The trigger-sensor
pair is the one most likely to be a real finding — if `serviceAllSensors()` genuinely never
services the trigger sensor, that is worth understanding before editing the test to agree
with it.

Work through them one at a time against `DutyCycleController.cpp`; do not bulk-update the
expected values to match current output.

---

## Failure 3 — stale `test/support/Arduino.cpp`

`test/support/Arduino.cpp` declares `FakeSerial Serial;` while `test/support/Arduino.h`
defines `inline FakeSerialForNative Serial;` — different types, same symbol. Unbuildable if
the test runner picks the `.cpp` up. It appears to be a leftover from before the header went
`inline`.

Fix: delete `test/support/Arduino.cpp` if nothing references `FakeSerial`, or reconcile it to
the header's type if something does. Check first — `grep -rn "FakeSerial" test/`.

Related and already fixed, recorded here only so it is not re-investigated:
`test/test_rtc_ticks/test_main.cpp` called `delay(2000)` without including anything that
declares it (that suite includes only `Samd21RtcTicks.h`, so it missed the shim other suites
pick up transitively through `FakeClock.h`). The `delay` call was removed during the RTC
sub-second work.

---

## Verification

```bash
cd platformio && pio test -e native
```

A clean compile is itself the main signal for Failure 1 and Failure 3, since both are build
errors rather than assertion failures. Failure 2 needs each of the six cases to pass on its
own merits.

Worth confirming while in here: `native`'s `build_src_filter` gained
`radio/TdmaRadioService.cpp` and `radio/TdmaTxQueue.cpp` during the RTC work. Check nothing
else the suites now reference is still missing from it.

## Open questions

- Is a second native env (`env:native_timed`, `-DSMARTFIRES_DUTY_CYCLE_MODE=2`) worth adding
  so the tripwire test can exercise a second `kActive*` branch directly instead of relying on
  raw-constant assertions as a proxy? Not blocking. Only worth it if more natively-testable
  duty-cycle logic accumulates — and note that with four modes, one extra env still leaves
  two branches uncovered, so the raw-constant assertions are the load-bearing part either way.
- `test/support/Test_Context.md:148` still references the removed `dutyCycleCfg()` factory and
  describes the real node build as running with "duty cycling disabled", which was already
  backwards before the mode rename. It needs rewriting against `SMARTFIRES_DUTY_CYCLE_MODE`
  once the test file itself is settled.
