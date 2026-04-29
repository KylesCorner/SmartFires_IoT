#include <unity.h>
#include <cstring>

#include "fakes/FakeClock.h"
#include "fakes/FakeSps30Driver.h"
#include "sensors/Sps30Sensor.h"

static void assertState(Sps30Sensor &sensor, SensorPowerState expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(sensor.powerState()));
}

void test_sps30_begin_success_starts_sleeping() {
  FakeClock clock;
  FakeSps30Driver driver;

  Sps30Sensor::Config cfg;
  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(driver.beginCalled);
  TEST_ASSERT_TRUE(sensor.healthy());
  assertState(sensor, SensorPowerState::Sleeping);
}

void test_sps30_begin_failure_enters_error() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.beginOk = false;

  Sps30Sensor::Config cfg;
  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.healthy());
  assertState(sensor, SensorPowerState::Error);
}

void test_sps30_wake_starts_measurement_and_waits_for_warmup() {
  FakeClock clock;
  FakeSps30Driver driver;

  Sps30Sensor::Config cfg;
  cfg.wakeDelayMs = 8000;

  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_EQUAL_UINT32(1, driver.startCount);
  assertState(sensor, SensorPowerState::Waking);

  clock.advance(7999);
  TEST_ASSERT_FALSE(sensor.service());

  clock.advance(1);
  TEST_ASSERT_TRUE(sensor.service());
  assertState(sensor, SensorPowerState::Ready);
}

void test_sps30_wake_failure_enters_error() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.startOk = false;

  Sps30Sensor::Config cfg;
  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.wake());

  TEST_ASSERT_FALSE(sensor.healthy());
  assertState(sensor, SensorPowerState::Error);
}

void test_sps30_sleep_stops_measurement() {
  FakeClock clock;
  FakeSps30Driver driver;

  Sps30Sensor::Config cfg;
  cfg.wakeDelayMs = 0;

  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL_UINT32(1, driver.stopCount);
  assertState(sensor, SensorPowerState::Sleeping);
}

void test_sps30_sleep_failure_enters_error() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.stopOk = false;

  Sps30Sensor::Config cfg;
  cfg.wakeDelayMs = 0;

  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.sleep());

  TEST_ASSERT_FALSE(sensor.healthy());
  assertState(sensor, SensorPowerState::Error);
}

void test_sps30_sample_valid_data() {
  FakeClock clock;
  FakeSps30Driver driver;

  driver.data.pm1_0 = 1.0f;
  driver.data.pm2_5 = 2.5f;
  driver.data.pm4_0 = 4.0f;
  driver.data.pm10_0 = 10.0f;
  driver.data.valid = true;

  Sps30Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 1000;

  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.ready());

  clock.advance(1000);

  TEST_ASSERT_TRUE(sensor.ready());
  TEST_ASSERT_TRUE(sensor.sample());

  const auto &r = sensor.reading();

  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, r.pm1_0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.5f, r.pm2_5);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 4.0f, r.pm4_0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, r.pm10_0);
  TEST_ASSERT_EQUAL_UINT32(1000, r.timestampMs);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
}

void test_sps30_invalid_data_fails_sample_but_remains_healthy() {
  FakeClock clock;
  FakeSps30Driver driver;

  driver.data.valid = false;

  Sps30Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 0;

  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_FALSE(sensor.reading().valid);
}

void test_sps30_read_failure_fails_sample() {
  FakeClock clock;
  FakeSps30Driver driver;
  driver.readOk = false;

  Sps30Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 0;

  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_TRUE(sensor.healthy());
}

void test_sps30_telemetry_writes_string() {
  FakeClock clock;
  FakeSps30Driver driver;

  driver.data.pm1_0 = 1.0f;
  driver.data.pm2_5 = 2.5f;
  driver.data.pm4_0 = 4.0f;
  driver.data.pm10_0 = 10.0f;
  driver.data.valid = true;

  Sps30Sensor::Config cfg;
  cfg.wakeDelayMs = 0;
  cfg.minSamplePeriodMs = 0;

  Sps30Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_TRUE(sensor.sample());

  char buf[160];
  const size_t n = sensor.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_NOT_NULL(strstr(buf, "sps30"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "pm1="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "pm25="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "pm10="));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_sps30_begin_success_starts_sleeping);
  RUN_TEST(test_sps30_begin_failure_enters_error);
  RUN_TEST(test_sps30_wake_starts_measurement_and_waits_for_warmup);
  RUN_TEST(test_sps30_wake_failure_enters_error);
  RUN_TEST(test_sps30_sleep_stops_measurement);
  RUN_TEST(test_sps30_sleep_failure_enters_error);
  RUN_TEST(test_sps30_sample_valid_data);
  RUN_TEST(test_sps30_invalid_data_fails_sample_but_remains_healthy);
  RUN_TEST(test_sps30_read_failure_fails_sample);
  RUN_TEST(test_sps30_telemetry_writes_string);

  return UNITY_END();
}
