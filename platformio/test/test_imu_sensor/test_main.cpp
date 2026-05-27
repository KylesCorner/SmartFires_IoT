#include <unity.h>

#include "sensors/Icm20948Sensor.h"

#include "fakes/FakeClock.h"
#include "fakes/FakeIcm20948Driver.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static Icm20948Sensor::Config makeCfg(
    uint32_t minSamplePeriodMs = 10,
    uint32_t wakeDelayMs = 0,
    SensorDutyClass dutyClass = SensorDutyClass::DutyCycled,
    uint8_t address = 0) {
  return Icm20948Sensor::Config::makeImuCfg(
      minSamplePeriodMs, wakeDelayMs, dutyClass, address);
}

static void wakeAndService(Icm20948Sensor &sensor, FakeClock &clock,
                           uint32_t nowMs = 100) {
  clock.set(nowMs);
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());
}

// -----------------------------------------------------------------------------
// begin()
// -----------------------------------------------------------------------------

void test_imu_name_returns_imu(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_EQUAL_STRING("imu", sensor.name());
}

void test_imu_begin_calls_driver_with_configured_address(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_EQUAL_UINT32(1, driver.beginCount);
  TEST_ASSERT_EQUAL_UINT8(0, driver.lastAddress);
}

void test_imu_begin_success_duty_cycled_enters_sleeping(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
  TEST_ASSERT_EQUAL(SensorDutyClass::DutyCycled, sensor.dutyClass());
}

void test_imu_begin_always_enters_sleeping_even_if_config_says_always_on(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::AlwaysOn, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
  TEST_ASSERT_EQUAL(SensorDutyClass::AlwaysOn, sensor.dutyClass());
}

void test_imu_begin_failure_enters_error(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.beginOk = false;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());

  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.ready());
}

// -----------------------------------------------------------------------------
// wake(), service(), sleep()
// -----------------------------------------------------------------------------

void test_imu_wake_fails_if_unhealthy(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.beginOk = false;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.wake());

  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

void test_imu_wake_enters_waking_state(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 25, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
}

void test_imu_service_does_not_enter_ready_before_wake_delay(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 25, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(24);

  TEST_ASSERT_FALSE(sensor.service());
  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
}

void test_imu_service_enters_ready_after_wake_delay(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 25, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(25);

  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_imu_zero_wake_delay_becomes_ready_on_service(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

void test_imu_service_fails_if_unhealthy(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.beginOk = false;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.service());

  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

void test_imu_sleep_duty_cycled_returns_to_sleeping(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());
  wakeAndService(sensor, clock);

  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());

  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
}

void test_imu_sleep_always_enters_sleeping_even_if_config_says_always_on(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::AlwaysOn, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock);

  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());

  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
}

void test_imu_sleep_fails_if_unhealthy(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.beginOk = false;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
}

// -----------------------------------------------------------------------------
// ready()
// -----------------------------------------------------------------------------

void test_imu_ready_false_before_begin(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  clock.set(1000);

  TEST_ASSERT_FALSE(sensor.ready());
}

void test_imu_ready_false_when_sleeping(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(1000);

  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.ready());
}

void test_imu_ready_true_after_wake_service_and_min_sample_period(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.ready());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_TRUE(sensor.ready());
}

// -----------------------------------------------------------------------------
// sample()
// -----------------------------------------------------------------------------

void test_imu_sample_fails_when_not_ready_and_does_not_read_driver(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.setReading(1, 2, 3, 4, 5, 6, 7, 8, 9, true);

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(0, driver.readCount);
  TEST_ASSERT_FALSE(sensor.reading().valid);
}

void test_imu_sample_success_after_wake_service_updates_reading(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.setReading(1.0f, 2.0f, 3.0f,
                    4.0f, 5.0f, 6.0f,
                    7.0f, 8.0f, 9.0f,
                    true);

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_TRUE(sensor.sample());

  const Icm20948Sensor::Reading &r = sensor.reading();

  TEST_ASSERT_TRUE(r.valid);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, r.accelX);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, r.accelY);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, r.accelZ);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f, r.gyroX);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, r.gyroY);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.0f, r.gyroZ);

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 7.0f, r.magX);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 8.0f, r.magY);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.0f, r.magZ);

  TEST_ASSERT_EQUAL_UINT32(100, r.timestampMs);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
}

void test_imu_sample_rate_limits_after_successful_sample(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.setReading(1, 2, 3, 4, 5, 6, 7, 8, 9, true);

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_TRUE(sensor.sample());

  clock.set(105);
  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);

  clock.set(110);
  TEST_ASSERT_TRUE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(2, driver.readCount);
}

void test_imu_sample_driver_read_failure_returns_false_but_stays_healthy(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.readOk = false;

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.reading().valid);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
}

void test_imu_sample_invalid_driver_data_returns_false_but_stays_healthy(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.setReading(1, 2, 3, 4, 5, 6, 7, 8, 9, false);

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
  TEST_ASSERT_FALSE(sensor.reading().valid);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
}

// -----------------------------------------------------------------------------
// readingData(), readingSize()
// -----------------------------------------------------------------------------

void test_imu_reading_data_returns_pointer_to_reading(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_EQUAL_PTR(&sensor.reading(), sensor.readingData());
}

void test_imu_reading_size_matches_reading_struct(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_EQUAL_UINT32(sizeof(Icm20948Sensor::Reading),
                           sensor.readingSize());
}

// -----------------------------------------------------------------------------
// telemetry
// -----------------------------------------------------------------------------

void test_imu_write_telemetry_returns_zero_for_null_buffer(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  TEST_ASSERT_EQUAL_UINT32(0, sensor.writeTelemetry(nullptr, 64));
}

void test_imu_write_telemetry_returns_zero_for_zero_size_buffer(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor sensor(makeCfg(), driver, clock);

  char out[8];

  TEST_ASSERT_EQUAL_UINT32(0, sensor.writeTelemetry(out, 0));
}

void test_imu_write_telemetry_formats_latest_reading_after_sample(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.setReading(1.25f, 2.25f, 3.25f,
                    4.25f, 5.25f, 6.25f,
                    7.25f, 8.25f, 9.25f,
                    true);

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_TRUE(sensor.sample());

  char out[256] = {};

  const size_t n = sensor.writeTelemetry(out, sizeof(out));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_EQUAL_STRING(
      "imu,ax=1.250,ay=2.250,az=3.250,gx=4.250,gy=5.250,gz=6.250,valid=1,t_ms=100",
      out);
}

void test_imu_write_telemetry_truncates_safely_after_sample(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.setReading(1, 2, 3, 4, 5, 6, 7, 8, 9, true);

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_TRUE(sensor.sample());

  char out[12] = {};

  const size_t n = sensor.writeTelemetry(out, sizeof(out));

  TEST_ASSERT_EQUAL_UINT32(sizeof(out) - 1, n);
  TEST_ASSERT_EQUAL_CHAR('\0', out[sizeof(out) - 1]);
}

void test_imu_fill_snapshot_preserves_driver_accel_mg_units(void) {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.setReading(-980.0f, 245.0f, 1012.0f,
                    4.0f, 5.0f, 6.0f,
                    -41.1f, 46.8f, 153.1f,
                    true);

  Icm20948Sensor sensor(makeCfg(10, 0, SensorDutyClass::DutyCycled, 0), driver,
                        clock);

  TEST_ASSERT_TRUE(sensor.begin());

  wakeAndService(sensor, clock, 100);

  TEST_ASSERT_TRUE(sensor.sample());

  SensorSnapshot snap = {};
  sensor.fillSnapshot(snap);

  TEST_ASSERT_TRUE(snap.imuValid);
  TEST_ASSERT_BITS_HIGH(0x08, snap.sensorFlags);
  TEST_ASSERT_EQUAL_INT16(-411, snap.magX);
  TEST_ASSERT_EQUAL_INT16(468, snap.magY);
  TEST_ASSERT_EQUAL_INT16(1531, snap.magZ);
  TEST_ASSERT_EQUAL_INT16(-980, snap.accelX);
  TEST_ASSERT_EQUAL_INT16(245, snap.accelY);
  TEST_ASSERT_EQUAL_INT16(1012, snap.accelZ);
}

// -----------------------------------------------------------------------------
// runner
// -----------------------------------------------------------------------------

void runIcm20948SensorTests() {
  UNITY_BEGIN();

  RUN_TEST(test_imu_name_returns_imu);

  RUN_TEST(test_imu_begin_calls_driver_with_configured_address);
  RUN_TEST(test_imu_begin_success_duty_cycled_enters_sleeping);
  RUN_TEST(test_imu_begin_always_enters_sleeping_even_if_config_says_always_on);
  RUN_TEST(test_imu_begin_failure_enters_error);

  RUN_TEST(test_imu_wake_fails_if_unhealthy);
  RUN_TEST(test_imu_wake_enters_waking_state);
  RUN_TEST(test_imu_service_does_not_enter_ready_before_wake_delay);
  RUN_TEST(test_imu_service_enters_ready_after_wake_delay);
  RUN_TEST(test_imu_zero_wake_delay_becomes_ready_on_service);
  RUN_TEST(test_imu_service_fails_if_unhealthy);
  RUN_TEST(test_imu_sleep_duty_cycled_returns_to_sleeping);
  RUN_TEST(test_imu_sleep_always_enters_sleeping_even_if_config_says_always_on);
  RUN_TEST(test_imu_sleep_fails_if_unhealthy);

  RUN_TEST(test_imu_ready_false_before_begin);
  RUN_TEST(test_imu_ready_false_when_sleeping);
  RUN_TEST(test_imu_ready_true_after_wake_service_and_min_sample_period);

  RUN_TEST(test_imu_sample_fails_when_not_ready_and_does_not_read_driver);
  RUN_TEST(test_imu_sample_success_after_wake_service_updates_reading);
  RUN_TEST(test_imu_sample_rate_limits_after_successful_sample);
  RUN_TEST(test_imu_sample_driver_read_failure_returns_false_but_stays_healthy);
  RUN_TEST(test_imu_sample_invalid_driver_data_returns_false_but_stays_healthy);

  RUN_TEST(test_imu_reading_data_returns_pointer_to_reading);
  RUN_TEST(test_imu_reading_size_matches_reading_struct);

  RUN_TEST(test_imu_write_telemetry_returns_zero_for_null_buffer);
  RUN_TEST(test_imu_write_telemetry_returns_zero_for_zero_size_buffer);
  RUN_TEST(test_imu_write_telemetry_formats_latest_reading_after_sample);
  RUN_TEST(test_imu_write_telemetry_truncates_safely_after_sample);
  RUN_TEST(test_imu_fill_snapshot_preserves_driver_accel_mg_units);

  UNITY_END();
}

#ifdef ARDUINO

void setup() {
  delay(2000);
  runIcm20948SensorTests();
}

void loop() {}

#else

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  runIcm20948SensorTests();

  return 0;
}

#endif
