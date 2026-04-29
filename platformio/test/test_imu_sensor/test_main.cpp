#include <unity.h>
#include <cstring>

#include "fakes/FakeClock.h"
#include "fakes/FakeIcm20948Driver.h"
#include "sensors/Icm20948Sensor.h"

static void assertState(Icm20948Sensor &sensor, SensorPowerState expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(sensor.powerState()));
}

void test_imu_begin_success_starts_sleeping() {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor::Config cfg;
  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(driver.beginCalled);
  TEST_ASSERT_TRUE(sensor.healthy());
  assertState(sensor, SensorPowerState::Sleeping);
}

void test_imu_begin_failure_enters_error() {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.beginOk = false;

  Icm20948Sensor::Config cfg;
  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.healthy());
  assertState(sensor, SensorPowerState::Error);
}

void test_imu_wake_delay_then_ready() {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor::Config cfg;
  cfg.wakeDelayMs = 25;

  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());

  assertState(sensor, SensorPowerState::Waking);

  clock.advance(24);
  TEST_ASSERT_FALSE(sensor.service());

  clock.advance(1);
  TEST_ASSERT_TRUE(sensor.service());

  assertState(sensor, SensorPowerState::Ready);
}

void test_imu_sleep_after_wake() {
  FakeClock clock;
  FakeIcm20948Driver driver;

  Icm20948Sensor::Config cfg;
  cfg.wakeDelayMs = 0;

  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_TRUE(sensor.sleep());

  assertState(sensor, SensorPowerState::Sleeping);
}

void test_imu_sample_valid_data() {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.data.accelX = 1.0f;
  driver.data.accelY = 2.0f;
  driver.data.accelZ = 3.0f;
  driver.data.gyroX = 4.0f;
  driver.data.gyroY = 5.0f;
  driver.data.gyroZ = 6.0f;
  driver.data.magX = 7.0f;
  driver.data.magY = 8.0f;
  driver.data.magZ = 9.0f;
  driver.data.valid = true;

  Icm20948Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 1000;

  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.ready());

  clock.advance(1000);

  TEST_ASSERT_TRUE(sensor.ready());
  TEST_ASSERT_TRUE(sensor.sample());

  const auto &r = sensor.reading();

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
  TEST_ASSERT_EQUAL_UINT32(1000, r.timestampMs);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
}

void test_imu_invalid_data_fails_sample_but_remains_healthy() {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.data.valid = false;

  Icm20948Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 0;

  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_FALSE(sensor.reading().valid);
}

void test_imu_read_failure_fails_sample() {
  FakeClock clock;
  FakeIcm20948Driver driver;
  driver.readOk = false;

  Icm20948Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 0;

  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_TRUE(sensor.healthy());
}

void test_imu_telemetry_writes_string() {
  FakeClock clock;
  FakeIcm20948Driver driver;

  driver.data.accelX = 1.0f;
  driver.data.accelY = 2.0f;
  driver.data.accelZ = 3.0f;
  driver.data.gyroX = 4.0f;
  driver.data.gyroY = 5.0f;
  driver.data.gyroZ = 6.0f;
  driver.data.valid = true;

  Icm20948Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 0;

  Icm20948Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_TRUE(sensor.sample());

  char buf[180];
  const size_t n = sensor.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_NOT_NULL(strstr(buf, "imu"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "ax="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "gx="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "valid=1"));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_imu_begin_success_starts_sleeping);
  RUN_TEST(test_imu_begin_failure_enters_error);
  RUN_TEST(test_imu_wake_delay_then_ready);
  RUN_TEST(test_imu_sleep_after_wake);
  RUN_TEST(test_imu_sample_valid_data);
  RUN_TEST(test_imu_invalid_data_fails_sample_but_remains_healthy);
  RUN_TEST(test_imu_read_failure_fails_sample);
  RUN_TEST(test_imu_telemetry_writes_string);

  return UNITY_END();
}
