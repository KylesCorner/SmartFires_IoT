---
name: gps-disciplined-clock
description: Deferred option to discipline each node's RTC rate from the PA1010D PPS signal.
category: plan-possible
status: deferred
related_docs:
  - rtc-subsecond-sleep
  - tdma-protocol
  - duty-cycling
---

# Possible: GPS-disciplined node clock

## Current state

The prerequisite RTC work is complete: node time runs from a continuously running SAMD21 RTC COUNT32 source through awake and standby periods. Network epoch and slot phase still come from LoRa `TIME_SYNC`, and the 20 ms guards tolerate current clock drift.

GPS PPS discipline itself is not implemented. The production GPS uses Backup power mode and is available only during normal sensing windows, so a design cannot assume continuous PPS.

## If resumed

- Confirm a free EIC-capable Feather pin and wire the PA1010D PPS output.
- Capture RTC ticks in a minimal interrupt handler and estimate rate error over multiple valid edges.
- Keep correction state RAM-only and retain nominal 1024 Hz behavior until a confident estimate exists.
- Reject missing, implausible, or stale edge sequences; a node without sky view must behave exactly as it does today.
- Feed the corrected rate through the existing clock abstraction without changing LoRa `TIME_SYNC` authority.
- Log correction and confidence, and gate the feature behind a build flag for field rollback.

Do not reduce the TDMA guard band as part of initial implementation. First compare raw and corrected elapsed time against an external reference and verify that intermittent GPS power does not make timing worse.

This work is optional unless drift measurements show that the existing guard and periodic synchronization are inadequate.
