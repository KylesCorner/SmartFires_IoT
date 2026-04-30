#include <unity.h>

#include "sensors/Sps30Sensor.h"

#include "fakes/FakeClock.h"
#include "fakes/FakeSps30Driver.h"

#include <string.h>

static Sps30Sensor::Config makeWarmupCfg(uint32_t minSamplePeriodMs = 100,
                                         uint32_t wakeDelayMs = 50) {
  return Sps30Sensor::Config::makeSps30Cfg(
      minSamplePeriodMs,
      wakeDelayMs,
      SensorDutyClass::WarmupHeavy);
}

static Sps30Sensor::Config makeDutyCycledCfg(uint32_t minSamplePeriodMs = 100,
                                             uint32_t wakeDelayMs = 50) {
  return Sps30Sensor::Config::makeSps30Cfg(
      minSamplePeriodMs,
      wakeDelayMs,
      SensorDutyClass::DutyCycled);
}

static void wakeAndServiceReady(Sps30Sensor &sensor, FakeClock &clock,
                                uint32_t startMs = 1000,
                                uint32_t wakeDelayMs = 50) {
  clock.set(startMs);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(wakeDelayMs);
  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_name_returns_sps30() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_EQUAL_STRING("sps30", sensor.name());
}

void test_duty_class_returns_configured_value() {
  FakeClock clock;
  FakeSps30Driver driver;

  Sps30Sensor warmupSensor(makeWarmupCfg(), driver, clock);
  TEST_ASSERT_EQUAL(SensorDutyClass::WarmupHeavy, warmupSensor.dutyClass());

  Sps30Sensor dutySensor(makeDutyCycledCfg(), driver, clock);
  TEST_ASSERT_EQUAL(SensorDutyClass::DutyCycled, dutySensor.dutyClass());
}

void test_begin_success_sets_sleeping_and_healthy() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_EQUAL_UINT32(1, driver.beginCount);
  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
}

void test_begin_failure_sets_error_and_unhealthy() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.beginOk = false;

  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());

  TEST_ASSERT_EQUAL_UINT32(1, driver.beginCount);
  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

void test_wake_returns_false_when_unhealthy() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.wake());
  TEST_ASSERT_EQUAL_UINT32(0, driver.startMeasurementCount);
}

void test_wake_success_starts_measurement_and_sets_waking() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1234);
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_EQUAL_UINT32(1, driver.startMeasurementCount);
  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
  TEST_ASSERT_TRUE(sensor.healthy());
}

void test_wake_is_idempotent_while_waking_or_ready() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_EQUAL_UINT32(1, driver.startMeasurementCount);
  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());

  clock.advance(50);
  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());

  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_EQUAL_UINT32(1, driver.startMeasurementCount);
}

void test_wake_failure_sets_error_and_unhealthy() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.startMeasurementOk = false;

  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.wake());

  TEST_ASSERT_EQUAL_UINT32(1, driver.startMeasurementCount);
  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

void test_service_returns_false_when_unhealthy() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.service());
}

void test_service_does_not_become_ready_before_wake_delay() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(49);
  TEST_ASSERT_FALSE(sensor.service());

  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
}

void test_service_becomes_ready_after_wake_delay() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(50);
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_sleep_returns_false_when_unhealthy() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.sleep());
  TEST_ASSERT_EQUAL_UINT32(0, driver.stopMeasurementCount);
}

void test_sleep_success_stops_measurement_and_sets_sleeping() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock);

  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL_UINT32(1, driver.stopMeasurementCount);
  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
  TEST_ASSERT_TRUE(sensor.healthy());
}

void test_sleep_is_idempotent_when_already_sleeping() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());

  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL_UINT32(0, driver.stopMeasurementCount);
  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
}

void test_sleep_failure_sets_error_and_unhealthy() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.stopMeasurementOk = false;

  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock);

  TEST_ASSERT_FALSE(sensor.sleep());

  TEST_ASSERT_EQUAL_UINT32(1, driver.stopMeasurementCount);
  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

void test_ready_false_before_begin() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  clock.set(1000);

  TEST_ASSERT_FALSE(sensor.ready());
}

void test_ready_false_while_sleeping() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);

  TEST_ASSERT_FALSE(sensor.ready());
}

void test_ready_false_while_waking() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(50 - 1);
  TEST_ASSERT_FALSE(sensor.service());
  TEST_ASSERT_FALSE(sensor.ready());
}

void test_ready_true_when_ready_and_min_sample_period_elapsed() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndServiceReady(sensor, clock, 1000, 50);

  TEST_ASSERT_TRUE(sensor.ready());
}

void test_sample_returns_false_and_does_not_read_when_not_ready() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.setReading(1.0f, 2.0f, 3.0f, 4.0f);

  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_EQUAL_UINT32(0, driver.readCount);
}

void test_sample_success_copies_reading_and_sets_timestamp() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.setReading(1.25f, 2.50f, 4.75f, 10.25f, true);

  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock, 1000, 50);

  TEST_ASSERT_TRUE(sensor.sample());

  const Sps30Sensor::Reading &r = sensor.reading();

  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.25f, r.pm1_0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.50f, r.pm2_5);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.75f, r.pm4_0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.25f, r.pm10_0);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(1050, r.timestampMs);
}

void test_sample_rate_limits_after_successful_sample() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.setReading(1.0f, 2.0f, 3.0f, 4.0f, true);

  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock, 1000, 50);

  TEST_ASSERT_TRUE(sensor.sample());
  TEST_ASSERT_FALSE(sensor.ready());

  clock.advance(99);
  TEST_ASSERT_FALSE(sensor.ready());

  clock.advance(1);
  TEST_ASSERT_TRUE(sensor.ready());
}

void test_sample_read_failure_marks_reading_invalid_but_stays_healthy_ready() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.readOk = false;

  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock, 1000, 50);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
  TEST_ASSERT_FALSE(sensor.reading().valid);
  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_sample_invalid_driver_data_marks_reading_invalid_but_stays_healthy_ready() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.setReading(1.0f, 2.0f, 3.0f, 4.0f, false);

  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock, 1000, 50);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
  TEST_ASSERT_FALSE(sensor.reading().valid);
  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_reading_data_and_size_match_reading() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_EQUAL_PTR(&sensor.reading(), sensor.readingData());
  TEST_ASSERT_EQUAL(sizeof(Sps30Sensor::Reading), sensor.readingSize());
}

void test_write_telemetry_returns_zero_for_null_buffer() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  TEST_ASSERT_EQUAL_UINT32(0, sensor.writeTelemetry(nullptr, 100));
}

void test_write_telemetry_returns_zero_for_zero_length_buffer() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  char buf[8];
  TEST_ASSERT_EQUAL_UINT32(0, sensor.writeTelemetry(buf, 0));
}

void test_write_telemetry_outputs_expected_format() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.setReading(1.25f, 2.50f, 4.75f, 10.25f, true);

  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock, 1000, 50);
  TEST_ASSERT_TRUE(sensor.sample());

  char buf[180];
  size_t n = sensor.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_EQUAL_STRING(
      "sps30,pm1=1.25,pm25=2.50,pm4=4.75,pm10=10.25,valid=1,t_ms=1050",
      buf);
}

void test_write_telemetry_truncates_safely() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.setReading(1.25f, 2.50f, 4.75f, 10.25f, true);

  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock, 1000, 50);
  TEST_ASSERT_TRUE(sensor.sample());

  char buf[8];
  memset(buf, 'X', sizeof(buf));

  size_t n = sensor.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_EQUAL_UINT32(sizeof(buf) - 1, n);
  TEST_ASSERT_EQUAL_CHAR('\0', buf[sizeof(buf) - 1]);
}

void test_fill_snapshot_ignores_invalid_reading() {
  FakeClock clock;
  FakeSps30Driver driver;
  Sps30Sensor sensor(makeWarmupCfg(), driver, clock);

  SensorSnapshot snap;
  snap.pm1_0 = 11.0f;
  snap.pm2_5 = 22.0f;
  snap.pm4_0 = 44.0f;
  snap.pm10 = 100.0f;
  snap.sensorFlags = 0;

  sensor.fillSnapshot(snap);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 11.0f, snap.pm1_0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 22.0f, snap.pm2_5);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 44.0f, snap.pm4_0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, snap.pm10);
  TEST_ASSERT_EQUAL_UINT16(0, snap.sensorFlags);
}

void test_fill_snapshot_writes_pm_fields_and_sets_flag_when_valid() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.setReading(1.25f, 2.50f, 4.75f, 10.25f, true);

  Sps30Sensor sensor(makeWarmupCfg(100, 50), driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndServiceReady(sensor, clock, 1000, 50);
  TEST_ASSERT_TRUE(sensor.sample());

  SensorSnapshot snap;
  snap.pm1_0 = 0.0f;
  snap.pm2_5 = 0.0f;
  snap.pm4_0 = 0.0f;
  snap.pm10 = 0.0f;
  snap.sensorFlags = 0;

  sensor.fillSnapshot(snap);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.25f, snap.pm1_0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.50f, snap.pm2_5);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.75f, snap.pm4_0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.25f, snap.pm10);
  TEST_ASSERT_TRUE((snap.sensorFlags & 0x10) != 0);
}

void runSps30SensorTests() {
  UNITY_BEGIN();

  RUN_TEST(test_name_returns_sps30);
  RUN_TEST(test_duty_class_returns_configured_value);

  RUN_TEST(test_begin_success_sets_sleeping_and_healthy);
  RUN_TEST(test_begin_failure_sets_error_and_unhealthy);

  RUN_TEST(test_wake_returns_false_when_unhealthy);
  RUN_TEST(test_wake_success_starts_measurement_and_sets_waking);
  RUN_TEST(test_wake_is_idempotent_while_waking_or_ready);
  RUN_TEST(test_wake_failure_sets_error_and_unhealthy);

  RUN_TEST(test_service_returns_false_when_unhealthy);
  RUN_TEST(test_service_does_not_become_ready_before_wake_delay);
  RUN_TEST(test_service_becomes_ready_after_wake_delay);

  RUN_TEST(test_sleep_returns_false_when_unhealthy);
  RUN_TEST(test_sleep_success_stops_measurement_and_sets_sleeping);
  RUN_TEST(test_sleep_is_idempotent_when_already_sleeping);
  RUN_TEST(test_sleep_failure_sets_error_and_unhealthy);

  RUN_TEST(test_ready_false_before_begin);
  RUN_TEST(test_ready_false_while_sleeping);
  RUN_TEST(test_ready_false_while_waking);
  RUN_TEST(test_ready_true_when_ready_and_min_sample_period_elapsed);

  RUN_TEST(test_sample_returns_false_and_does_not_read_when_not_ready);
  RUN_TEST(test_sample_success_copies_reading_and_sets_timestamp);
  RUN_TEST(test_sample_rate_limits_after_successful_sample);
  RUN_TEST(test_sample_read_failure_marks_reading_invalid_but_stays_healthy_ready);
  RUN_TEST(test_sample_invalid_driver_data_marks_reading_invalid_but_stays_healthy_ready);

  RUN_TEST(test_reading_data_and_size_match_reading);

  RUN_TEST(test_write_telemetry_returns_zero_for_null_buffer);
  RUN_TEST(test_write_telemetry_returns_zero_for_zero_length_buffer);
  RUN_TEST(test_write_telemetry_outputs_expected_format);
  RUN_TEST(test_write_telemetry_truncates_safely);

  RUN_TEST(test_fill_snapshot_ignores_invalid_reading);
  RUN_TEST(test_fill_snapshot_writes_pm_fields_and_sets_flag_when_valid);

  UNITY_END();
}

#ifdef ARDUINO
void setup() {
  delay(2000);
  runSps30SensorTests();
}

void loop() {}
#else
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  runSps30SensorTests();
  return 0;
}
#endif
