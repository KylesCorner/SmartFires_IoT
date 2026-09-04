---
name: reset-reason-diagnostics
description: Remaining hardware validation for shipped AWAKEN reset-cause and retained hang-zone diagnostics.
category: plan-pending
status: draft
related_docs:
  - watchdog-timer
  - reset-system
  - packet-reliability
  - jetson-bridge
---

# Reset reason diagnostics: remaining work

## Status at 2026-09-04

Wire transport, retained breadcrumb, firmware instrumentation, edge persistence, and dashboard display are implemented. The outstanding work is real-hardware validation; optional program-counter capture remains deliberately deferred.

Current wire format:

```text
AWAKEN = PktHeader(5) + AwakenPayload(6) + CRC(1) = 12 bytes

AwakenPayload:
  uid_hash:u32
  reset_cause:u8
  hang_zone:u8
```

The firmware and Python decoders also accept the legacy 9-byte form (four-byte legacy header, UID hash only, CRC). Legacy events expose no reset/hang values.

## Shipped implementation

- `ResetDiagnostics` reads raw SAMD21 `PM->RCAUSE` at boot.
- A `.noinit` breadcrumb retains magic, hang zone, inverse-zone guard, and boot count across warm reset.
- Node targets use `flash_with_bootloader_noinit.ld` and `SMARTFIRES_RESET_DIAG`.
- `harvest()` trusts the previous hang zone only for a watchdog reset with an intact breadcrumb, then prepares the new boot record.
- Instrumented zones are boot, radio TX/wait paths, SHT31 I2C, GPS I2C, IMU I2C, SPS30 UART, and normal loop idle.
- `SmartFiresNodeApp` places reset cause and harvested hang zone in every AWAKEN.
- The base decodes/logs assignment context and forwards the complete frame.
- Python maps RCAUSE bits and zone values, persists them in telemetry/status records, and serves reboot events to the dashboard history page.
- The normal reset system is also shipped: hard/soft node reset, base reset through node ID 0, and session-start base soft reset.

Current architecture and wire-table docs now describe the 12-byte frame; the earlier documentation-update task is complete.

## Meaning and limits

`reset_cause` can include WDT, BOD33/BOD12, external reset, system reset, and power-on bits. Treat it as a bit field, not a single exclusive enum.

`hang_zone` is attribution evidence only when a WDT reset occurred and the retained record passed integrity checks. Otherwise it must be `ZONE_UNKNOWN`. It identifies the last marked blocking region, not an exact line or proven root cause. A stale or overly broad zone can still misattribute the underlying failure.

The breadcrumb does not survive a true power loss and is not designed to. Cold-boot SRAM is ignored unless the validity guards and WDT condition agree.

## Required hardware validation

### Cold boot and ordinary reset

1. Power-cycle a node and confirm AWAKEN shows a power-on cause with unknown hang zone.
2. Double-tap into the bootloader and confirm flashing/boot remains reliable with the custom linker script.
3. Issue soft and hard `CMD_RESET`; confirm the reported cause, new AWAKEN, reassignment, and resumed session are coherent.
4. Verify a non-WDT reset never reports a trusted prior hang zone.

### Watchdog retention

Add a temporary, controlled test hook that stops feeding the watchdog inside one known zone at a time. Do not simulate a WDT by calling `NVIC_SystemReset()`, because that tests a different cause.

For each available zone:

1. mark/enter the zone;
2. induce the hang until hardware WDT reset;
3. capture local boot log and forwarded AWAKEN;
4. verify WDT is present in `reset_cause`;
5. verify `hang_zone` equals the induced region;
6. verify the dashboard and stored row display the same values;
7. verify telemetry resumes after assignment.

At minimum test radio TX, one I2C path, SPS30 UART, and loop idle. Repeat enough times to detect intermittent retention/bootloader behavior.

### Corruption and compatibility

- Corrupt or invalidate breadcrumb guards in a test build and verify the zone becomes UNKNOWN.
- Run a legacy AWAKEN fixture through base/edge decode and verify no length-failure flood.
- Test a diagnostics node against the current base/edge and a legacy fixture against current decoders.
- Confirm CSV field lists accept reset fields without raising `DictWriter` extras errors.

### Standby interaction

Timed MCU standby disables the watchdog today. Decide whether to add a dedicated `ZONE_MCU_STANDBY` only together with the work in `STANDBY_WATCHDOG_COVERAGE.md`; a zone without watchdog coverage would not produce the event it claims to diagnose.

## Deferred phase: program-counter capture

Exact PC/stack capture would require a WDT early-warning interrupt and likely bypassing part of Adafruit SleepyDog. It adds ISR, retention, stack-unwind, and linker complexity. Do not start it unless zone-level hardware results are insufficient to identify the dominant hang class.

If pursued, specify register capture integrity, nested-fault behavior, symbolization workflow, memory cost, and compatibility with the bootloader/noinit layout before implementation.

## Completion criteria

Move this document to `Completed_Plans/` when:

- cold boot, hard/soft reset, and bootloader tests pass;
- induced WDT resets preserve and report the expected zones;
- legacy and corruption tests pass;
- persisted/dashboard values match firmware logs;
- standby-zone handling is explicitly resolved or deferred;
- test hooks are removed or isolated behind an off-by-default diagnostic flag.
