# SmartFires native-test context

This file is a concise maintainer reference for tests under `platformio/test/`. Current production behavior is documented in `documentation/Current_Architecture/`; known test debt is tracked in `documentation/Possible_Plans/NATIVE_TEST_REPAIR.md`.

## Rules

- Use the shared `native` environment and Unity.
- Keep production behavior unchanged unless a test demonstrates a real bug and the requested change includes fixing it.
- Test each sensor according to its own duty class and driver semantics; do not force all sensors into one lifecycle.
- Inject `IClock`, driver interfaces, `ITdmaRadioDriver`, and trigger sources instead of importing Arduino hardware libraries.
- Prefer public behavior over private implementation details.
- Put reusable doubles in `test/support/fakes/`.
- Use wrap-safe unsigned-time cases for every millisecond timer.
- Keep packet tests synchronized with both C++ `BinaryPacket.h` and Python `packet.py`.

## Active deployment assumptions

- `NUM_SLOTS=5`: base slot 0 plus four assignable node slots.
- Node IDs start at 2 and arrive through out-of-band `AWAKEN` plus direct `TIME_SYNC`.
- Current node environments use `AppLayerAckSummary` reliability and a 15-second STATUS build flag.
- Production is SensorTriggered; debug/timed targets are Timed; hybrid is separately selectable.
- Timed mode is 10 s warmup, 30 samples at 1 s, two full bundles, nominal 35 s standby in a 75 s period.
- RX gating wakes 150 ms before base slot 0 in the operating profile. Some unit tests intentionally create local configs with other values to test boundary math.
- Commands are fire-and-forget from the base and acknowledged by `CMD_ACK`; `ACK_SUMMARY` and direct sync still use selected link ACK paths.

Do not copy fallback member defaults from `TdmaConfig` and call them “current.” Production composition uses `NetworkConfig::nodeTdmaProfile()`.

## Native source filter

`platformio.ini` builds these production sources for native tests:

```text
power/
sensors/
radio/TdmaClock.cpp
radio/PacketHandler.cpp
radio/TdmaRadioService.cpp
radio/TdmaTxQueue.cpp
radio/TxPowerController.cpp
```

Entrypoints and Arduino platform adapters are excluded. When a suite has undefined references, check whether its concrete `.cpp` is in this filter before changing code or tests.

## Test doubles

- `FakeClock`: explicit `set()`/`advance()` millisecond time.
- `FakeSensor` and sensor-specific driver fakes: lifecycle, validity, and filled snapshot data.
- `FakeTriggerSensor`: controlled threshold polling and wake conditions.
- `FakeTdmaRadioDriver`: sent packet capture, received-packet injection, ACK behavior, sleep/wake state, and failure injection.
- `FakeAnalogReader`: battery/wind ADC behavior.

Fakes should be deterministic and expose only observations a real caller can make. Avoid hidden wall-clock time or global state.

## High-value boundaries

### Duty cycle

Cover wake/warmup/sample/cooldown/sleep transitions, exact threshold edges, Timed full-bundle hold and overrun ceiling, derived standby floor, MCU sleep/drain coordination, trigger polling, and sample errors with `failOnSampleError=false`.

Do not restore removed `dutyCycleCfg()`/`dutyCycleCfgContinuous()` factories. Construct `DutyCycleConfig::make(...)` or use `SensingConfig::DutyCycle` constants that match the behavior under test.

### TDMA and reliability

Cover both slot guards, frame/sequence wrap, pre-sync and stale-sync fallbacks, 150 ms operating receive wake-ahead, one-retry-per-slot, `WINDOW_BEGIN` retry priority, queue/pending eviction, 9-second retry gate, summary bitmap application, manual packet-type ACK rules, radio sleep, and TX budget deferral.

### Packets

Assert current sizes and CRC behavior: BUNDLE max 195, STATUS 27, AWAKEN 12/legacy 9, TIME_SYNC 14, ACK_SUMMARY 10, window marker 17, calibrate/reset 8, set-power 9, command ACK 12. Test sequence and timestamp reconstruction, delta clamps, legacy decode paths, and invalid/truncated data.

### TX power

Feed explicit times and SNR samples. Cover first STATUS baseline, minute pacing, low-margin jump to baseline, clean-headroom 2 dBm step-down, retry/failure inhibition, STATIC mode, in-flight command gate/timeout, AWAKEN reset, stale reconciliation through STATUS, and bounded silence probes.

### Sensors

Test begin/wake/sleep/service/sample/ready/healthy/`fillSnapshot()` according to the individual sensor's actual interface. A valid sample must set the correct `SensorSnapshot` fields/flags; an unavailable reading must not masquerade as zero-valued valid data.

## Runner pattern

Suites normally use a desktop `main()`:

```cpp
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_something);
  return UNITY_END();
}
```

Keep setup/teardown local to the suite. Include `<cstdio>`/`<stdio.h>` if a fake uses `snprintf`; silence intentionally unused interface parameters with `(void)param` rather than weakening warnings.

## Current known failures

As of the documentation audit, do not expect the full environment to pass. `Possible_Plans/NATIVE_TEST_REPAIR.md` records the deferred repair groups:

- `test_config` references removed duty-cycle factories;
- six duty-controller expectations no longer match the current implementation;
- `test/support/Arduino.cpp` duplicates inline shim definitions;
- the TxPowerController source-filter omission was corrected in this consolidation but still needs a PlatformIO verification run.

When repair work and execution are authorized, record the exact command, compiler/linker failures, failing assertions, and whether the result was already listed in the possible-plan note.
