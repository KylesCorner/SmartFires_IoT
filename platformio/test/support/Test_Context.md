# SmartFires Native Unit Testing Context

## Project

SmartFires IoT / PlatformIO / native Unity tests.

I am building native PlatformIO unit tests for SmartFires sensors, drivers, and power logic.

The goal is to keep future tests consistent across sensors, while still testing each sensor according to its actual production behavior.

Do not force every sensor to behave the same way. Some sensors are `AlwaysOn`, some are `DutyCycled`, and some intentionally have special behavior.

---

## Main Testing Rules

- Use one shared native PlatformIO environment for all tests.
- Use `pio test`, not `pio run`, for test execution.
- Keep production behavior unchanged unless a test reveals a real bug and I explicitly ask to change it.
- Build tests around actual production behavior.
- Prefer full drop-in test files when possible.
- Put reusable test fakes in `test/support/fakes/`.
- Use `FakeClock` for time-based behavior.
- Avoid pulling Arduino hardware libraries into native tests.
- Prefer testing sensors through injected driver interfaces.
- Test public behavior, not private internals.
- Do not “fix” sensor behavior just because it differs from another sensor.
- Keep the documented firmware behavior aligned with the current network model: 4 total TDMA entities, out-of-band `AWAKEN`, direct `TIME_SYNC` assignment, and continuous sampling when duty cycling is disabled.

---

## Native Test Environment

Use one shared native environment:

```ini
[env:native]
platform = native
test_framework = unity
test_build_src = yes

build_flags =
  ${env.build_flags}
  -DUNIT_TEST
  -DSMARTFIRES_NATIVE_TEST=1
  -Itest/support

build_src_filter =
  +<power/>
  +<sensors/>
  -<main.cpp>
  -<main_base.cpp>
  -<main_node.cpp>
  -<main_node_dummy.cpp>
```

Run individual test suites like:

```bash
pio test -e native -f test_duty_cycle_controller
pio test -e native -f test_sht31_sensor
pio test -e native -f test_imu_sensor
pio test -e native -f test_gps_sensor
pio test -e native -f test_sps30_sensor
```

---

## Test Support Layout

Shared support files live under:

```text
test/support/
  Arduino.h
  Arduino.cpp
  fakes/
    FakeClock.h
    FakeSensor.h
    FakeTriggerSensor.h
    FakeSht31Driver.h
    FakeIcm20948Driver.h
    FakeGpsDriver.h
```

The native Arduino shim exists because `platform = native` does not provide `Arduino.h`.

The shim provides stubs for things like:

- `delay()`
- `millis()` if needed
- `Serial.print()`
- `Serial.println()`
- `pinMode()`
- other small Arduino compatibility functions

---

## FakeClock

Use the shared `FakeClock` for all time-based tests.

```cpp
#pragma once

#include <Arduino.h>
#include "interfaces/IClock.h"

class FakeClock : public IClock {
public:
  uint32_t nowMs = 0;

  uint32_t millis() const override {
    return nowMs;
  }

  void set(uint32_t ms) {
    nowMs = ms;
  }

  void advance(uint32_t ms) {
    nowMs += ms;
  }
};
```

Common use:

```cpp
FakeClock clock;

clock.set(100);
TEST_ASSERT_TRUE(sensor.ready());

clock.advance(50);
TEST_ASSERT_FALSE(sensor.ready());
```

## Current Regression To Keep Covered

The normal node build uses `DutyCycleConfig::dutyCycleCfg()` with duty cycling disabled. In that mode, the controller must stay in `ActiveSampling` and continue sampling instead of transitioning into cooldown after `activeSampleMs`.

Any future change to `DutyCycleController` should preserve that behavior unless the runtime design is intentionally changed.

---

## Common Fake Gotcha

If a fake uses `snprintf()`, include one of these:

```cpp
#include <cstdio>
```

or:

```cpp
#include <stdio.h>
```

Otherwise native compilation can fail with:

```text
error: ‘snprintf’ was not declared in this scope
```

This came up with `FakeSensor.h`.

---

## Harmless ISensor Warning

This warning is harmless:

```cpp
virtual void fillSnapshot(SensorSnapshot &snap) const {}
```

Warning:

```text
unused parameter ‘snap’
```

It can be silenced with:

```cpp
virtual void fillSnapshot(SensorSnapshot &snap) const {
  (void)snap;
}
```

This is not a blocking test failure.

---

## Standard Unity Test Runner Pattern

Most current SmartFires native tests use a simple `main()` runner:

```cpp
int main() {
  delay(2000);
  UNITY_BEGIN();

  RUN_TEST(test_something);

  UNITY_END();
  return 0;
}
```

Using the cross-platform `setup()` / `loop()` wrapper is still fine when needed, but keep the runner style consistent within a given test file.

---

## Generic Sensor Test Pattern

For each new sensor, create a test folder:

```text
test/test_<sensor_name>_sensor/
  test_main.cpp
```

Create any reusable fake driver under:

```text
test/support/fakes/Fake<SensorOrDriverName>.h
```

Example:

```text
test/support/fakes/FakeGpsDriver.h
test/support/fakes/FakeSht31Driver.h
test/support/fakes/FakeIcm20948Driver.h
```

---

## What a Sensor Fake Should Usually Track

A fake driver should usually expose:

```cpp
bool beginOk = true;
bool readOk = true;
bool pollOk = true;

uint8_t lastAddress = 0;

uint32_t beginCount = 0;
uint32_t readCount = 0;
uint32_t pollCount = 0;
```

Only include fields that make sense for that driver.

For example:

- GPS needs `pollCount` because GPS has `poll()`.
- SHT31 does not need `pollCount`.
- IMU needs `readCount`.
- A trigger fake may need `sampleCount` and manually settable readings.

---

## Generic Sensor Test Checklist

Most sensor tests should cover:

### Construction / Identity

- `name()` returns the expected sensor name.
- `dutyClass()` returns the configured duty class.

### Begin

- `begin()` succeeds when the driver succeeds.
- `begin()` fails when the driver fails.
- configured I2C address is passed into the driver.
- successful begin sets the expected initial power state.
- failed begin sets `SensorPowerState::Error` if that is the production behavior.
- failed begin sets `healthy() == false`.

### Wake / Sleep / Service

- `wake()` returns false if the sensor is unhealthy.
- `sleep()` returns false if the sensor is unhealthy.
- `wake()` sets the expected state.
- `sleep()` sets the expected state.
- `service()` does any required polling or warmup work.
- `service()` moves from `Waking` to `Ready` after `wakeDelayMs`, if the sensor supports wake delay.
- `service()` does not move to `Ready` before `wakeDelayMs`.

### Ready

- `ready()` is false before begin or when unhealthy.
- `ready()` is false unless the sensor is in the correct power state.
- `ready()` respects `minSamplePeriodMs`.
- after a successful sample, `ready()` should be false until enough time passes.

### Sample

- `sample()` returns false if not ready.
- `sample()` does not call the driver when not ready.
- `sample()` calls the driver when ready.
- successful sample copies driver data into the sensor reading.
- successful sample sets `timestampMs`.
- successful sample updates rate limiting time.
- failed driver read behavior matches production behavior:
  - Some sensors mark themselves unhealthy or enter Error.
  - Some sensors stay healthy and Ready.
  - Do not assume all sensors handle read failure the same way.

### Integration / State Machine Notes

- `PacketHandler` bundle emission is buffered; repeated snapshot creation should not be interpreted as a bundle-per-sample contract.
- `ACK_SUMMARY ack_base_seq=N mask=0x0` means contiguous acknowledgment through `N` with no additional out-of-order packets acknowledged above `N`.

### Reading Access

- `reading()` returns the latest reading.
- `readingData()` returns a pointer to the reading.
- `readingSize()` returns `sizeof(Reading)`.

### Telemetry

- `writeTelemetry()` returns nonzero for a valid buffer.
- `writeTelemetry(nullptr, maxLen)` returns `0`.
- `writeTelemetry(out, 0)` returns `0`.
- telemetry format matches actual production format.
- truncation does not overflow and leaves the output null-terminated.

### Snapshot / Optional APIs

If the sensor implements snapshot behavior:

- `fillSnapshot()` writes the expected fields.
- invalid readings should not corrupt snapshot data.

---

## Generic Config Helper Pattern

Use small helpers in test files to keep tests readable.

Example:

```cpp
static SomeSensor::Config makeAlwaysOnCfg(
    uint32_t minSamplePeriodMs = 100,
    uint32_t wakeDelayMs = 0,
    uint8_t address = 0x10) {
  return SomeSensor::Config::makeCfg(
      minSamplePeriodMs,
      wakeDelayMs,
      SensorDutyClass::AlwaysOn,
      address);
}
```

Example for a duty-cycled sensor:

```cpp
static SomeSensor::Config makeDutyCycledCfg(
    uint32_t minSamplePeriodMs = 100,
    uint32_t wakeDelayMs = 50,
    uint8_t address = 0x10) {
  return SomeSensor::Config::makeCfg(
      minSamplePeriodMs,
      wakeDelayMs,
      SensorDutyClass::DutyCycled,
      address);
}
```

Use the actual config factory name from the production sensor.

---

## DutyCycleController Testing Context

The `DutyCycleController` tests use:

- `FakeClock`
- `FakeSensor`
- `FakeTriggerSensor`

`DutyCycleController` now depends on `ITriggerSensor` instead of directly on `Sht31Sensor`.

This made threshold-trigger tests easier and more generic.

---

## ITriggerSensor Shape

```cpp
class ITriggerSensor {
public:
  struct Reading {
    bool valid = false;
    float tempC = 0.0f;
    float humidityPct = 0.0f;
  };

  virtual ~ITriggerSensor() = default;

  virtual bool service() = 0;
  virtual bool ready() const = 0;
  virtual bool sample() = 0;
  virtual const Reading &triggerReading() const = 0;
};
```

Important:

`ITriggerSensor::service()` returns `bool`, not `void`, because `Sht31Sensor` also inherits `ISensor`, where `service()` returns `bool`.

---

## FakeTriggerSensor

The fake lets tests set trigger readings like:

```cpp
trigger.setReading(tempC, humidityPct, valid);
```

Use it to simulate threshold crossing without depending on the real SHT31 sensor.

---

## FakeSensor

`FakeSensor` implements `ISensor`.

It tracks:

- `beginCount`
- `serviceCount`
- `sampleCount`
- `sleepCount`
- `wakeCount`

It has knobs:

- `beginOk`
- `serviceOk`
- `sampleOk`
- `sleepOk`
- `wakeOk`
- `readyValue`

`FakeSensor` must implement all current `ISensor` pure virtual methods:

```cpp
healthy() const
powerState() const
dutyClass() const
readingData() const
readingSize() const
writeTelemetry(char *out, size_t maxLen) const
```

---

## SHT31 Sensor Testing Context

### SHT31 Test Files

```text
test/test_sht31_sensor/test_main.cpp
test/support/fakes/FakeSht31Driver.h
test/support/fakes/FakeClock.h
```

---

## FakeSht31Driver

`FakeSht31Driver` implements `ISht31Driver`.

It tracks:

- `beginOk`
- `lastAddress`
- `beginCount`
- `readTempCount`
- `readHumidityCount`
- `tempC`
- `humidityPct`

---

## SHT31 Production Behavior

`Sht31Sensor` respects `SensorDutyClass`.

Current behavior:

- `begin()` calls `_driver.begin(_cfg.address)`.
- if begin succeeds and sensor is `AlwaysOn`, state becomes `Ready`.
- if begin succeeds and sensor is `DutyCycled`, state becomes `Sleeping`.
- if begin fails, state becomes `Error`.
- `sleep()` returns `Ready` for `AlwaysOn`.
- `sleep()` returns `Sleeping` for `DutyCycled`.
- `sample()` sets `Error` / unhealthy if temp or humidity is `NaN`.

---

## SHT31 Tests Cover

- begin success/failure
- configured I2C address
- wake/service/sleep behavior
- `ready()` min sample period
- sample success
- sample rate limiting
- NaN read failure
- `triggerReading()`
- `readingData()` / `readingSize()`
- `writeTelemetry()`
- `fillSnapshot()`

---

## IMU Sensor Testing Context

### IMU Test Files

```text
test/test_imu_sensor/test_main.cpp
test/support/fakes/FakeIcm20948Driver.h
test/support/fakes/FakeClock.h
```

---

## FakeIcm20948Driver

`FakeIcm20948Driver` implements `IIcm20948Driver`.

It tracks:

- `beginOk`
- `readOk`
- `lastAddress`
- `beginCount`
- `readCount`
- `Data data`

It has:

```cpp
void setReading(float ax, float ay, float az,
                float gx, float gy, float gz,
                float mx, float my, float mz,
                bool valid = true);
```

---

## IMU Production Behavior

The IMU is intentionally duty-cycled in current code.

Even if config says `AlwaysOn`, current `Icm20948Sensor.cpp` still begins in `Sleeping` and `sleep()` sets `Sleeping`.

Do not “fix” production code to match SHT31 unless explicitly asked.

Current behavior:

- `begin()` calls `_driver.begin(_cfg.address)`.
- if begin succeeds, `_state = SensorPowerState::Sleeping`.
- if begin fails, `_state = SensorPowerState::Error`.
- `wake()` sets `Waking`.
- `service()` moves `Waking` to `Ready` after `wakeDelayMs`.
- `sleep()` always sets `Sleeping`.
- `ready()` requires healthy, state `Ready`, and min sample period elapsed.
- `sample()` returns false if not ready.
- `sample()` calls `driver.read(data)`.
- if `driver.read()` fails, sample returns false but does not mark unhealthy or Error.
- if `data.valid` is false, sample returns false but does not mark unhealthy or Error.
- successful sample copies `IIcm20948Driver::Data` into reading and sets timestamp.
- `writeTelemetry()` outputs accel and gyro only, not magnetometer.

---

## IMU Telemetry Format

Actual format:

```cpp
"imu,ax=%.3f,ay=%.3f,az=%.3f,gx=%.3f,gy=%.3f,gz=%.3f,valid=%u,t_ms=%lu"
```

Expected example:

```text
imu,ax=1.250,ay=2.250,az=3.250,gx=4.250,gy=5.250,gz=6.250,valid=1,t_ms=100
```

---

## IMU Test Adjustments

IMU tests had to be adjusted to match duty-cycled behavior:

- tests should call `wake()` and `service()` before expecting ready/sample success.
- do not assume `AlwaysOn` starts `Ready`.
- failed driver read should return false but sensor stays healthy/Ready.
- invalid driver data should return false but sensor stays healthy/Ready.

Helper used in IMU tests:

```cpp
static void wakeAndService(Icm20948Sensor &sensor, FakeClock &clock,
                           uint32_t nowMs = 100) {
  clock.set(nowMs);
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());
}
```

---

## GPS Sensor Testing Context

### GPS Test Files

```text
test/test_gps_sensor/test_main.cpp
test/support/fakes/FakeGpsDriver.h
test/support/fakes/FakeClock.h
```

---

## GPS Interface

`IGpsDriver` shape:

```cpp
class IGpsDriver {
public:
  struct Data {
    bool fix = false;
    uint8_t fixQuality = 0;
    uint8_t satellites = 0;

    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
    float altitudeM = 0.0f;

    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
  };

  virtual ~IGpsDriver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual bool poll() = 0;
  virtual bool read(Data &out) = 0;
};
```

---

## FakeGpsDriver

`FakeGpsDriver` should implement `IGpsDriver`.

Recommended fake state:

```cpp
bool beginOk = true;
bool pollOk = true;
bool readOk = true;

uint8_t lastAddress = 0;

uint32_t beginCount = 0;
uint32_t pollCount = 0;
uint32_t readCount = 0;

Data data;
```

Recommended helper methods:

```cpp
void setFix(float lat, float lon, float alt,
            uint8_t sats = 7,
            uint8_t fixQuality = 1,
            uint8_t hour = 12,
            uint8_t minute = 34,
            uint8_t second = 56);

void setNoFix(uint8_t hour = 1,
              uint8_t minute = 2,
              uint8_t second = 3);
```

---

## GPS Production Behavior

Current `Pa1010dGpsSensor` behavior:

- `begin()` calls `_driver.begin(_cfg.address)`.
- if begin succeeds:
  - `_healthy = true`
  - `_state = SensorPowerState::Ready`
  - `_lastSampleMs = 0`
- if begin fails:
  - `_healthy = false`
  - `_state = SensorPowerState::Error`

### wake()

- returns false if unhealthy.
- if `AlwaysOn`, sets `Ready`.
- if `DutyCycled`, records `_wakeStartMs` and sets `Waking`.

### sleep()

- returns false if unhealthy.
- if `AlwaysOn`, sets `Ready`.
- if `DutyCycled`, sets `Sleeping`.

### service()

- returns false if unhealthy.
- always calls `_driver.poll()`.
- returns false if `poll()` fails.
- if state is `Waking` and `wakeDelayMs` has elapsed, sets `Ready`.
- returns true only when state is `Ready`.

### ready()

Requires:

- healthy
- state is `Ready`
- `_clock.millis() - _lastSampleMs >= _cfg.minSamplePeriodMs`

### sample()

- returns false if not ready.
- calls `_driver.read(data)`.
- if read fails:
  - sets `_reading.valid = false`
  - returns false
  - does not mark sensor unhealthy
  - does not set Error
- if read succeeds:
  - copies `IGpsDriver::Data` into `Reading`
  - sets `_reading.valid = data.fix`
  - sets `_reading.timestampMs = _clock.millis()`
  - sets `_lastSampleMs = _clock.millis()`
  - returns true, even when `data.fix` is false

### reading access

- `readingData()` returns `&_reading`.
- `readingSize()` returns `sizeof(Reading)`.

---

## Important GPS Behavior Difference

For GPS, a successful driver read with no GPS fix is still a successful sample.

In that case:

- `sample()` returns true.
- `reading.fix` is false.
- `reading.valid` is false.

This is different from sensors where invalid physical data may make `sample()` fail.

---

## GPS Tests Should Cover

- begin success/failure
- configured I2C address
- `name()` returns `"gps"`
- `dutyClass()` returns configured duty class
- `AlwaysOn` wake/sleep both leave sensor `Ready`
- `DutyCycled` sleep sets `Sleeping`
- `DutyCycled` wake sets `Waking`
- `service()` calls `driver.poll()`
- `service()` returns false if `poll()` fails
- `DutyCycled` service does not become `Ready` before `wakeDelayMs`
- `DutyCycled` service becomes `Ready` after `wakeDelayMs`
- `ready()` respects `minSamplePeriodMs`
- `sample()` returns false and does not call `driver.read()` when not ready
- `sample()` with fix copies GPS data and sets `valid = true`
- `sample()` without fix copies GPS time/no-fix data and sets `valid = false`, but returns true
- driver read failure returns false, marks reading invalid, but leaves sensor healthy and `Ready`
- sample rate limiting after successful sample
- `readingData()` / `readingSize()`
- `writeTelemetry()`
- `writeTelemetry()` null buffer behavior
- `writeTelemetry()` zero-length buffer behavior
- `writeTelemetry()` truncation behavior

---

## GPS Telemetry Format

Actual format:

```cpp
"gps,fix=%u,fixq=%u,sats=%u,lat=%.6f,lon=%.6f,alt=%.2f,t=%02u:%02u:%02u,valid=%u,t_ms=%lu"
```

Expected native example may look like:

```text
gps,fix=1,fixq=1,sats=8,lat=46.872101,lon=-113.994003,alt=978.50,t=12:34:56,valid=1,t_ms=100
```

---

## GPS Telemetry Test Gotcha

Exact telemetry string tests for GPS can be fragile because C/C++ float precision may print values like:

```text
46.872100f  -> 46.872101
-113.994000f -> -113.994003
```

If an exact string comparison fails only in the last decimal place, switch the test to stable substring checks or use values that are easier to compare exactly.

---

## GPS Config Helper Pattern

```cpp
static Pa1010dGpsSensor::Config makeAlwaysOnCfg(
    uint32_t minSamplePeriodMs = 100,
    uint32_t wakeDelayMs = 0,
    uint8_t address = 0x10) {
  return Pa1010dGpsSensor::Config::makeGpsCfg(
      minSamplePeriodMs,
      wakeDelayMs,
      SensorDutyClass::AlwaysOn,
      address);
}
```

```cpp
static Pa1010dGpsSensor::Config makeDutyCycledCfg(
    uint32_t minSamplePeriodMs = 100,
    uint32_t wakeDelayMs = 50,
    uint8_t address = 0x10) {
  return Pa1010dGpsSensor::Config::makeGpsCfg(
      minSamplePeriodMs,
      wakeDelayMs,
      SensorDutyClass::DutyCycled,
      address);
}
```

---

## Recommended Process For Adding A New Sensor Test

## 1. Identify the production driver interface

Example interfaces:

```text
ISht31Driver
IIcm20948Driver
IGpsDriver
```

If the sensor does not already have a driver interface, consider adding one before writing native tests.

The production sensor should depend on the interface, not directly on the hardware library.

---

## 2. Create a fake driver

Place it under:

```text
test/support/fakes/
```

Name it like:

```text
Fake<SensorOrDriverName>.h
```

Example:

```text
FakeGpsDriver.h
```

The fake should:

- implement the driver interface
- expose success/failure knobs
- store the last configured address
- count calls
- expose fake reading data
- provide helper methods for setting readings

---

## 3. Create the test folder

```text
test/test_<sensor_name>_sensor/
  test_main.cpp
```

Example:

```text
test/test_gps_sensor/test_main.cpp
```

---

## 4. Add small config helpers

Use helper functions like:

```cpp
makeAlwaysOnCfg()
makeDutyCycledCfg()
```

This keeps individual tests short.

---

## 5. Test begin behavior first

Start with:

- begin success
- begin failure
- configured address
- healthy state
- initial power state

---

## 6. Test power state behavior

Then test:

- wake
- sleep
- service
- wake delay
- unhealthy behavior

Use `FakeClock`.

---

## 7. Test sampling behavior

Then test:

- not ready
- ready
- successful sample
- failed driver read
- invalid reading behavior
- timestamp
- rate limiting

Important:

Do not assume failed sample means unhealthy. Match the specific production sensor.

---

## 8. Test data access and telemetry

Finally test:

- `reading()`
- `readingData()`
- `readingSize()`
- `writeTelemetry()`
- null buffer
- zero-length buffer
- truncation

---

## Future Test Guidance

When adding more sensor tests, keep this pattern:

- one fake driver per injected hardware interface
- one `test/test_<sensor>_sensor/test_main.cpp` per sensor
- use `FakeClock` for time
- use call counts to prove the driver was or was not called
- test production behavior exactly as it exists
- only change production code if the behavior is truly wrong
- prefer full drop-in test files
- avoid Arduino hardware dependencies in native tests

Good future test command pattern:

```bash
pio test -e native -f test_<sensor_name>_sensor
```

Example:

```bash
pio test -e native -f test_gps_sensor
```
