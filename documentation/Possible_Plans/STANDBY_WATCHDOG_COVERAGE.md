---
name: standby-watchdog-coverage
description: Deferred decision about watchdog coverage while a Timed node is in MCU standby.
category: plan-possible
status: deferred
related_docs:
  - watchdog-timer
  - duty-cycling
  - mcu-duty-cycle-changelog
  - rtc-subsecond-sleep
  - reset-reason-diagnostics
---

# Possible: watchdog coverage during standby

## Current gap

`Samd21RtcSleep::sleepFor()` disables the watchdog before standby and restores it after wake. A normal Timed cycle sleeps for about 35 of 75 seconds, so a failure to leave standby has no watchdog recovery during that interval.

The former source comment referring to a five-minute standby was stale and has been corrected. Hybrid has the five-minute backstop but does not enter MCU standby; the active Timed constraint is about 35 seconds.

## Options if resumed

1. Accept and document the bounded gap if field evidence shows standby wake failures are not occurring.
2. Confirm the real maximum SAMD21/Adafruit SleepyDog timeout. If it cannot span 35 seconds, keeping the WDT enabled requires segmented sleep.
3. Split standby into RTC-alarm segments and pet the watchdog between them. Measure whether the extra wakes materially affect cycle energy.
4. Coordinate any direct WDT/early-warning work with more precise reset diagnostics rather than implementing two register-level WDT paths.

## Verification

- Add an off-by-default induced-standby-hang test and confirm the node reboots instead of remaining dark.
- Add a `ZONE_MCU_STANDBY` breadcrumb only if watchdog recovery is actually active during standby.
- Confirm segmented sleep, if chosen, preserves planned sleep and the 75-second wake-to-wake period.
- Measure extra wake energy using the existing power-test setup.

This remains a risk/energy tradeoff, not required work for the current deployment.
