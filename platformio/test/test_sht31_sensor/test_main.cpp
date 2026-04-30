#include <unity.h>

#include "sensors/Sht31Sensor.h"

#include "fakes/FakeClock.h"
#include "fakes/FakeSht31Driver.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static Sht31Sensor::Config makeCfg(
    uint8_t address = 0x45,
    uint32_t minSamplePeriodMs = 1000,
    uint32_t wakeDelayMs = 15,
    SensorDutyClass dutyClass = SensorDutyClass::AlwaysOn) {
  return Sht31Sensor::Config::makeSht31Cfg(
      address, minSamplePeriodMs, wakeDelayMs, dutyClass);
}

// -----------------------------------------------------------------------------
// begin()
// -----------------------------------------------------------------------------

void test_begin_calls_driver_with_configured_address(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(makeCfg(0x44), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_EQUAL_UINT32(1, driver.beginCount);
  TEST_ASSERT_EQUAL_UINT8(0x44, driver.lastAddress);
}

void test_begin_success_always_on_enters_ready(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
  TEST_ASSERT_EQUAL(SensorDutyClass::AlwaysOn, sensor.dutyClass());
}

void test_begin_success_duty_cycled_enters_sleeping(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::DutyCycled),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
  TEST_ASSERT_EQUAL(SensorDutyClass::DutyCycled, sensor.dutyClass());
}

void test_begin_failure_enters_error(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.beginOk = false;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());

  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.ready());
}

// -----------------------------------------------------------------------------
// wake(), service(), sleep()
// -----------------------------------------------------------------------------

void test_wake_fails_if_sensor_is_unhealthy(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.beginOk = false;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.wake());

  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

void test_wake_enters_waking_state(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::DutyCycled),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
}

void test_service_does_not_enter_ready_before_wake_delay(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::DutyCycled),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(14);

  TEST_ASSERT_FALSE(sensor.service());
  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
}

void test_service_enters_ready_after_wake_delay(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::DutyCycled),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(15);

  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_service_fails_if_sensor_is_unhealthy(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.beginOk = false;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.service());

  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

void test_sleep_always_on_returns_to_ready(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_sleep_duty_cycled_returns_to_sleeping(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::DutyCycled),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(15);
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
}

void test_sleep_fails_if_sensor_is_unhealthy(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.beginOk = false;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

// -----------------------------------------------------------------------------
// ready()
// -----------------------------------------------------------------------------

void test_ready_false_before_begin(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  clock.set(1000);

  TEST_ASSERT_FALSE(sensor.ready());
}

void test_ready_false_until_min_sample_period_elapsed(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(999);
  TEST_ASSERT_FALSE(sensor.ready());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.ready());
}

void test_ready_false_when_sleeping(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::DutyCycled),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(5000);

  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.ready());
}

// -----------------------------------------------------------------------------
// sample()
// -----------------------------------------------------------------------------

void test_sample_fails_when_not_ready_and_does_not_read_driver(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(999);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(0, driver.readTempCount);
  TEST_ASSERT_EQUAL_UINT32(0, driver.readHumidityCount);
  TEST_ASSERT_FALSE(sensor.reading().valid);
}

void test_sample_success_updates_reading(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.setReading(23.25f, 51.5f);

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);

  TEST_ASSERT_TRUE(sensor.sample());

  const Sht31Sensor::Reading &r = sensor.reading();

  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 23.25f, r.tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 51.5f, r.humidityPct);
  TEST_ASSERT_EQUAL_UINT32(1000, r.timestampMs);

  TEST_ASSERT_EQUAL_UINT32(1, driver.readTempCount);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readHumidityCount);
}

void test_sample_success_updates_trigger_reading(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.setReading(24.0f, 48.0f);

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);

  TEST_ASSERT_TRUE(sensor.sample());

  const ITriggerSensor::Reading &tr = sensor.triggerReading();

  TEST_ASSERT_TRUE(tr.valid);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 24.0f, tr.tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 48.0f, tr.humidityPct);
}

void test_sample_rate_limits_second_sample(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.sample());

  clock.set(1500);
  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(1, driver.readTempCount);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readHumidityCount);

  clock.set(2000);
  TEST_ASSERT_TRUE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(2, driver.readTempCount);
  TEST_ASSERT_EQUAL_UINT32(2, driver.readHumidityCount);
}

void test_sample_nan_temperature_enters_error(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.setReading(NAN, 50.0f);

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.reading().valid);
  TEST_ASSERT_FALSE(sensor.triggerReading().valid);
}

void test_sample_nan_humidity_enters_error(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.setReading(22.0f, NAN);

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.reading().valid);
  TEST_ASSERT_FALSE(sensor.triggerReading().valid);
}

// -----------------------------------------------------------------------------
// data access
// -----------------------------------------------------------------------------

void test_reading_data_returns_pointer_to_reading(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_EQUAL_PTR(&sensor.reading(), sensor.readingData());
}

void test_reading_size_matches_reading_struct(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_EQUAL_UINT32(sizeof(Sht31Sensor::Reading), sensor.readingSize());
}

// -----------------------------------------------------------------------------
// telemetry
// -----------------------------------------------------------------------------

void test_write_telemetry_returns_zero_for_null_buffer(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_EQUAL_UINT32(0, sensor.writeTelemetry(nullptr, 64));
}

void test_write_telemetry_returns_zero_for_zero_size_buffer(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  char out[8];

  TEST_ASSERT_EQUAL_UINT32(0, sensor.writeTelemetry(out, 0));
}

void test_write_telemetry_formats_latest_reading(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.setReading(24.125f, 48.875f);

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.sample());

  char out[128] = {};

  const size_t n = sensor.writeTelemetry(out, sizeof(out));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_EQUAL_STRING(
      "sht31,temp_c=24.12,humidity_pct=48.88,valid=1,t_ms=1000",
      out);
}

void test_write_telemetry_truncates_safely(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.setReading(24.0f, 50.0f);

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.sample());

  char out[12] = {};

  const size_t n = sensor.writeTelemetry(out, sizeof(out));

  TEST_ASSERT_EQUAL_UINT32(sizeof(out) - 1, n);
  TEST_ASSERT_EQUAL_CHAR('\0', out[sizeof(out) - 1]);
}

// -----------------------------------------------------------------------------
// snapshot
// -----------------------------------------------------------------------------

void test_fill_snapshot_does_nothing_when_reading_invalid(void) {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor sensor(makeCfg(), driver, clock);

  SensorSnapshot snap{};
  snap.tempC = -99.0f;
  snap.humidityPct = -99.0f;
  snap.sensorFlags = 0;

  sensor.fillSnapshot(snap);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, -99.0f, snap.tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -99.0f, snap.humidityPct);
  TEST_ASSERT_EQUAL_UINT16(0, snap.sensorFlags);
}

void test_fill_snapshot_sets_temp_humidity_and_flag_when_reading_valid(void) {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.setReading(26.0f, 41.0f);

  Sht31Sensor sensor(
      makeCfg(0x45, 1000, 15, SensorDutyClass::AlwaysOn),
      driver,
      clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.sample());

  SensorSnapshot snap{};
  snap.sensorFlags = 0;

  sensor.fillSnapshot(snap);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 26.0f, snap.tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 41.0f, snap.humidityPct);
  TEST_ASSERT_BITS_HIGH(0x02, snap.sensorFlags);
}

// -----------------------------------------------------------------------------
// runner
// -----------------------------------------------------------------------------

void runSht31SensorTests() {
  UNITY_BEGIN();

  RUN_TEST(test_begin_calls_driver_with_configured_address);
  RUN_TEST(test_begin_success_always_on_enters_ready);
  RUN_TEST(test_begin_success_duty_cycled_enters_sleeping);
  RUN_TEST(test_begin_failure_enters_error);

  RUN_TEST(test_wake_fails_if_sensor_is_unhealthy);
  RUN_TEST(test_wake_enters_waking_state);
  RUN_TEST(test_service_does_not_enter_ready_before_wake_delay);
  RUN_TEST(test_service_enters_ready_after_wake_delay);
  RUN_TEST(test_service_fails_if_sensor_is_unhealthy);
  RUN_TEST(test_sleep_always_on_returns_to_ready);
  RUN_TEST(test_sleep_duty_cycled_returns_to_sleeping);
  RUN_TEST(test_sleep_fails_if_sensor_is_unhealthy);

  RUN_TEST(test_ready_false_before_begin);
  RUN_TEST(test_ready_false_until_min_sample_period_elapsed);
  RUN_TEST(test_ready_false_when_sleeping);

  RUN_TEST(test_sample_fails_when_not_ready_and_does_not_read_driver);
  RUN_TEST(test_sample_success_updates_reading);
  RUN_TEST(test_sample_success_updates_trigger_reading);
  RUN_TEST(test_sample_rate_limits_second_sample);
  RUN_TEST(test_sample_nan_temperature_enters_error);
  RUN_TEST(test_sample_nan_humidity_enters_error);

  RUN_TEST(test_reading_data_returns_pointer_to_reading);
  RUN_TEST(test_reading_size_matches_reading_struct);

  RUN_TEST(test_write_telemetry_returns_zero_for_null_buffer);
  RUN_TEST(test_write_telemetry_returns_zero_for_zero_size_buffer);
  RUN_TEST(test_write_telemetry_formats_latest_reading);
  RUN_TEST(test_write_telemetry_truncates_safely);

  RUN_TEST(test_fill_snapshot_does_nothing_when_reading_invalid);
  RUN_TEST(test_fill_snapshot_sets_temp_humidity_and_flag_when_reading_valid);

  UNITY_END();
}

#ifdef ARDUINO

void setup() {
  delay(2000);
  runSht31SensorTests();
}

void loop() {}

#else

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  runSht31SensorTests();

  return 0;
}

#endif
