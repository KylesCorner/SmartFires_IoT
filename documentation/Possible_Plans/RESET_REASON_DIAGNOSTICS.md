---
name: reset-reason-diagnostics
description: Deferred hardware validation and optional precision improvements for shipped reset diagnostics.
category: plan-possible
status: deferred
related_docs:
  - watchdog-timer
  - reset-system
  - packet-reliability
  - jetson-bridge
---

# Possible: validate reset-reason diagnostics

## Current state

The implementation is shipped. Nodes read SAMD21 reset-cause bits, retain an integrity-guarded hang-zone breadcrumb across warm reset, include both fields in the 12-byte AWAKEN packet, and forward them through edge persistence and the dashboard. Current decoders also accept legacy 9-byte AWAKEN packets.

The remaining work is hardware validation, not missing transport or UI code.

## If resumed

- Power-cycle and hard/soft-reset a node; verify cause bits, reassignment, and resumed telemetry.
- Use an off-by-default test hook to induce a real watchdog reset in representative radio, I2C, SPS30 UART, and loop-idle zones.
- Confirm only an intact breadcrumb paired with a WDT cause yields a trusted hang zone.
- Verify the same values in local logs, forwarded AWAKEN, stored rows, and dashboard history.
- Confirm the custom `.noinit` linker layout does not interfere with bootloader entry or flashing.
- Retain legacy-frame and corrupt-breadcrumb compatibility checks.

Exact program-counter/stack capture would require WDT early-warning and substantially more retention/linker machinery. Revisit it only if zone-level data proves insufficient.
