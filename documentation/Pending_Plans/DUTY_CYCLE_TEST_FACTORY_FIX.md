---
name: duty-cycle-test-factory-fix
description: Fix the native test_config tripwire test, which still calls the removed DutyCycleConfig::dutyCycleCfgContinuous()/dutyCycleCfg() factories and no longer compiles; align it with the kActive* build-flag-selected constant set introduced for SMARTFIRES_DUTY_CYCLE_CONTINUOUS.
category: plan-pending
status: draft
related_docs:
  - duty-cycling
  - tunable-parameters
---

# Fix Duty Cycle Config Test Drift

## Background

`platformio/test/test_config/test_main.cpp` is a "tripwire" test (see its own header comment)
whose job is to catch a config wrapper silently drifting from the named constants in
`config/*.h`. Two of its test functions reference factory functions that no longer exist:

```cpp
// test_main.cpp:115-123
void test_duty_cycle_continuous_matches_sensing_config(void) {
  DutyCycleConfig cfg = DutyCycleConfig::dutyCycleCfgContinuous();   // <-- doesn't exist
  ...
}

// test_main.cpp:125-135
void test_duty_cycle_threshold_matches_sensing_config(void) {
  DutyCycleConfig cfg = DutyCycleConfig::dutyCycleCfg();             // <-- doesn't exist
  ...
}
```

`include/power/DutyCycleController.h` only defines a single factory today,
`DutyCycleConfig::make(...)` (see `DutyCycleConfig::make` in that header) — the
`dutyCycleCfgContinuous()`/`dutyCycleCfg()` pair was already removed by an earlier
consolidation. `DUTY_CYCLING.md` and `TUNABLE_PARAMETERS.md` both already correctly say "there
are no separate factories," but this test file was never updated to match, so
**`pio test -e native` almost certainly fails to compile today**, independent of any other
change.

This was discovered while wiring up `SMARTFIRES_DUTY_CYCLE_CONTINUOUS` (see `duty-cycling`),
which added a third, derived constant set — `SensingConfig::DutyCycle::kActive*` — that
`main.cpp` now wires into `DutyCycleConfig::make(...)` instead of hardcoding `kThreshold*` or
`kContinuous*` directly. The broken test predates that change and is unrelated to it, but the
new `kActive*` set is the natural thing to fix it against.

A related doc, `platformio/test/support/Test_Context.md:148`, is also now stale in two ways:
it references the same removed `dutyCycleCfg()` factory, and it describes "the normal node
build" as running with "duty cycling disabled" — which was true before the
`SMARTFIRES_DUTY_CYCLE_CONTINUOUS` change but is backwards now (the real node env,
`feather_m0_lora_node`, sets the flag to `0`, selecting `kThreshold*` with
`kThresholdEnabled = true`).

## Goal

Restore the tripwire test's actual purpose — catching a config wrapper that's drifted from
`config/SensingConfig.h` — using only API that currently exists, and bring `Test_Context.md`'s
description back in line with current behavior.

## Plan

1. **Replace the two broken test functions** in `test_config/test_main.cpp` with one,
   `test_duty_cycle_active_profile_matches_sensing_config`, that mirrors what `main.cpp`
   actually does (`main.cpp:202-211`):

   ```cpp
   void test_duty_cycle_active_profile_matches_sensing_config(void) {
     DutyCycleConfig cfg = DutyCycleConfig::make(
         SensingConfig::DutyCycle::kActiveEnabled,
         SensingConfig::DutyCycle::kActiveMinSleepMs,
         SensingConfig::DutyCycle::kActiveMaxWakeMs,
         SensingConfig::DutyCycle::kActiveActiveSampleMs,
         SensingConfig::DutyCycle::kActiveSamplePeriodMs,
         SensingConfig::DutyCycle::kActiveWarmupMs,
         SensingConfig::DutyCycle::kActiveTempDeltaThresholdC,
         SensingConfig::DutyCycle::kActiveHumidityDeltaThresholdPct,
         SensingConfig::DutyCycle::kActiveFailOnSampleError);

     TEST_ASSERT_EQUAL(SensingConfig::DutyCycle::kActiveEnabled, cfg.enabled);
     TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kActiveMinSleepMs, cfg.minSleepMs);
     TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kActiveMaxWakeMs, cfg.maxWakeMs);
     TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kActiveActiveSampleMs, cfg.activeSampleMs);
     TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kActiveSamplePeriodMs, cfg.samplePeriodMs);
     TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kActiveWarmupMs, cfg.warmupMs);
     TEST_ASSERT_EQUAL_FLOAT(SensingConfig::DutyCycle::kActiveTempDeltaThresholdC,
                              cfg.tempDeltaThresholdC);
     TEST_ASSERT_EQUAL_FLOAT(SensingConfig::DutyCycle::kActiveHumidityDeltaThresholdPct,
                              cfg.humidityDeltaThresholdPct);
     TEST_ASSERT_EQUAL(SensingConfig::DutyCycle::kActiveFailOnSampleError, cfg.failOnSampleError);
   }
   ```

   This is the only test function that needs `RUN_TEST(...)` updated to match (remove the two
   old `RUN_TEST` calls, add the one new one — check `test_main.cpp`'s `main()`/`setup()` for
   the exact `RUN_TEST` list).

2. **Important caveat to document in the test's comment**: `[env:native]` never defines
   `SMARTFIRES_DUTY_CYCLE_CONTINUOUS`, so under `SensingConfig.h`'s `#ifndef` guard it always
   resolves to `1` (continuous) for the native build. This test will therefore only ever
   exercise the `kContinuous*` branch of `kActive*` — it cannot, as written, catch a drift in
   the `kThreshold*` branch, because that branch is never compiled under `native`. Say this
   explicitly in a comment above the test so a future reader doesn't assume both branches are
   covered.

3. **Add two cheap, always-compiled assertions** that don't depend on which branch `kActive*`
   resolves to, so a regression in the *other* branch's raw values is still caught even though
   `kActive*` won't reflect them under native:

   ```cpp
   TEST_ASSERT_TRUE(SensingConfig::DutyCycle::kThresholdEnabled);
   TEST_ASSERT_FALSE(SensingConfig::DutyCycle::kContinuousEnabled);
   ```

   These pin the two raw flags themselves (not just whichever one `kActive*` happens to alias
   today), so flipping either one by accident still fails a native test even without a second
   build variant.

4. **Fix `test/support/Test_Context.md:148`** — replace the `dutyCycleCfg()` reference and the
   now-backwards "duty cycling disabled" claim. Something like:

   > The real node build (`feather_m0_lora_node`, `SMARTFIRES_DUTY_CYCLE_CONTINUOUS=0`) runs
   > with duty cycling **enabled** (`kThresholdEnabled = true`) — the controller cycles through
   > `IdleSleeping`/`WarmingUp`/`ActiveSampling`/`CooldownSleeping`. The debug build
   > (`feather_m0_lora_node_debug`, `SMARTFIRES_DUTY_CYCLE_CONTINUOUS=1`, also native's default)
   > runs with it **disabled** — the controller must stay in `ActiveSampling` and keep sampling
   > instead of transitioning into cooldown after `activeSampleMs`. Any future change to
   > `DutyCycleController` should preserve both behaviors.

5. **Sweep for other stale references** — already checked: `grep -rn "dutyCycleCfg"` across the
   repo turns up only this test file and three docs (`DUTY_CYCLING.md`, `TUNABLE_PARAMETERS.md`
   — both already correct/updated; `Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md`,
   which is a historical plan doc and can be left as-is per the `Completed_Plans/` convention
   of not editing history).

## Verification

- `pio run -e native -t test` (or `pio test -e native`) should compile and all `test_config`
  cases should pass — this is currently blocked by the missing factories, so a clean compile is
  itself the main signal this plan worked.

## Open questions

- Is it worth adding a second native test environment (e.g. `env:native_threshold`) that sets
  `-DSMARTFIRES_DUTY_CYCLE_CONTINUOUS=0` purely to let
  `test_duty_cycle_active_profile_matches_sensing_config` exercise the `kThreshold*` branch
  directly under native, rather than relying on the field-level assertions in step 3 as a proxy?
  Not blocking this fix; only worth it if this area accumulates more native-testable duty-cycle
  logic later.
