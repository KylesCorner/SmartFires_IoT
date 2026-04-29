#include <unity.h>
#include <cstring>

#include "fakes/FakeClock.h"
#include "fakes/FakeSht31Driver.h"
#include "sensors/Sht31Sensor.h"

static void assertState(Sht31Sensor &sensor, SensorPowerState expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(sensor.powerState()));
}

void test_sht31_begin_success_starts_sleeping_for_duty_cycled() {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor::Config cfg;
  cfg.address = 0x45;
  cfg.dutyClass = SensorDutyClass::DutyCycled;

  Sht31Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(driver.beginCalled);
  TEST_ASSERT_EQUAL_UINT8(0x45, driver.lastBeginAddress);
  TEST_ASSERT_TRUE(sensor.healthy());
  assertState(sensor, SensorPowerState::Sleeping);
}

void test_sht31_begin_failure_enters_error() {
  FakeClock clock;
  FakeSht31Driver driver;
  driver.beginOk = false;

  Sht31Sensor::Config cfg;
  Sht31Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.healthy());
  assertState(sensor, SensorPowerState::Error);
}

void test_sht31_wake_delay_then_sample() {
  FakeClock clock;
  FakeSht31Driver driver;

  driver.tempC = 21.25f;
  driver.humidityPct = 44.5f;

  Sht31Sensor::Config cfg;
  cfg.wakeDelayMs = 15;
  cfg.minSamplePeriodMs = 1000;

  Sht31Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.wake());
  assertState(sensor, SensorPowerState::Waking);

  clock.advance(14);
  TEST_ASSERT_FALSE(sensor.service());

  clock.advance(1);
  TEST_ASSERT_TRUE(sensor.service());
  assertState(sensor, SensorPowerState::Ready);

  TEST_ASSERT_FALSE(sensor.ready());

  clock.advance(1000);
  TEST_ASSERT_TRUE(sensor.ready());

  TEST_ASSERT_TRUE(sensor.sample());

  const auto &r = sensor.reading();

  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.25f, r.tempC);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 44.5f, r.humidityPct);
  TEST_ASSERT_EQUAL_UINT32(1015, r.timestampMs);
}
void test_sht31_nan_sample_enters_error() {
  FakeClock clock;
  FakeSht31Driver driver;

  driver.tempC = NAN;
  driver.humidityPct = 40.0f;

  Sht31Sensor::Config cfg;
  cfg.minSamplePeriodMs = 0;
  cfg.wakeDelayMs = 0;

  Sht31Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_FALSE(sensor.healthy());
  assertState(sensor, SensorPowerState::Error);
}

void test_sht31_telemetry_writes_string() {
  FakeClock clock;
  FakeSht31Driver driver;

  Sht31Sensor::Config cfg;
  cfg.minSamplePeriodMs = 0;
  cfg.wakeDelayMs = 0;

  Sht31Sensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_TRUE(sensor.sample());

  char buf[128];
  const size_t n = sensor.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_NOT_NULL(strstr(buf, "sht31"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "temp_c="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "humidity_pct="));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_sht31_begin_success_starts_sleeping_for_duty_cycled);
  RUN_TEST(test_sht31_begin_failure_enters_error);
  RUN_TEST(test_sht31_wake_delay_then_sample);
  RUN_TEST(test_sht31_nan_sample_enters_error);
  RUN_TEST(test_sht31_telemetry_writes_string);

  return UNITY_END();
}
