---
name: native-test-repair
description: Deferred repair list for the currently red native PlatformIO test environment.
category: plan-possible
status: deferred
related_docs:
  - duty-cycling
  - tunable-parameters
  - window-marker-packets
  - rtc-subsecond-sleep
  - mcu-duty-cycle-changelog
---

# Possible: repair the native test suite

## Known state

The full `pio test -e native` suite is not expected to pass. It has not been executed during the documentation consolidation because repository guidance requires explicit authorization for PlatformIO commands.

Known repair groups:

1. `test_config` calls removed `DutyCycleConfig` factories. Replace those calls with the current `DutyCycleConfig::make(...)` API and `SensingConfig::DutyCycle` values.
2. Six duty-controller assertions need individual review against production behavior. Do not update expected values in bulk; the trigger-service cases may reveal a real behavior gap.
3. `test/support/Arduino.cpp` conflicts with the inline serial shim in `Arduino.h`; remove or reconcile the obsolete implementation after confirming references.

The native source filter now includes `TxPowerController.cpp`, along with the production power, sensor, TDMA, queue, and packet-handler sources needed by current suites.

## Completion check

When this work is resumed, run from `platformio/`:

```bash
pio test -e native
```

Require a clean build and green assertions, document any production bug found while reviewing the six failures, and keep `platformio/test/README` and `test/support/Test_Context.md` synchronized with the result.
