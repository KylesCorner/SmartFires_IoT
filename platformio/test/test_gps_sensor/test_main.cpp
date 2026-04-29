#include <unity.h>
#include <cstring>

#include "fakes/FakeClock.h"
#include "fakes/FakeGpsDriver.h"
#include "sensors/Pa1010dGpsSensor.h"

static void assertState(Pa1010dGpsSensor &sensor, SensorPowerState expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(sensor.powerState()));
}

void test_gps_begin_success_starts_ready_for_always_on() {
  FakeClock clock;
  FakeGpsDriver driver;

  Pa1010dGpsSensor::Config cfg;
  cfg.dutyClass = SensorDutyClass::AlwaysOn;

  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(driver.beginCalled);
  TEST_ASSERT_TRUE(sensor.healthy());
  assertState(sensor, SensorPowerState::Ready);
}

void test_gps_begin_failure_enters_error() {
  FakeClock clock;
  FakeGpsDriver driver;
  driver.beginOk = false;

  Pa1010dGpsSensor::Config cfg;
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.healthy());
  assertState(sensor, SensorPowerState::Error);
}

void test_gps_service_polls_driver() {
  FakeClock clock;
  FakeGpsDriver driver;

  Pa1010dGpsSensor::Config cfg;
  cfg.dutyClass = SensorDutyClass::AlwaysOn;

  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_EQUAL_UINT32(1, driver.pollCount);

  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_EQUAL_UINT32(2, driver.pollCount);
}

void test_gps_sample_with_fix() {
  FakeClock clock;
  FakeGpsDriver driver;

  driver.data.fix = true;
  driver.data.fixQuality = 1;
  driver.data.satellites = 8;
  driver.data.latitudeDeg = 46.8721f;
  driver.data.longitudeDeg = -113.9940f;
  driver.data.altitudeM = 978.5f;
  driver.data.hour = 12;
  driver.data.minute = 34;
  driver.data.second = 56;

  Pa1010dGpsSensor::Config cfg;
  cfg.dutyClass = SensorDutyClass::AlwaysOn;
  cfg.minSamplePeriodMs = 1000;

  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.ready());

  clock.advance(1000);

  TEST_ASSERT_TRUE(sensor.ready());
  TEST_ASSERT_TRUE(sensor.sample());

  const auto &r = sensor.reading();

  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.fix);
  TEST_ASSERT_EQUAL_UINT8(1, r.fixQuality);
  TEST_ASSERT_EQUAL_UINT8(8, r.satellites);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 46.8721f, r.latitudeDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -113.9940f, r.longitudeDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 978.5f, r.altitudeM);
  TEST_ASSERT_EQUAL_UINT8(12, r.hour);
  TEST_ASSERT_EQUAL_UINT8(34, r.minute);
  TEST_ASSERT_EQUAL_UINT8(56, r.second);
  TEST_ASSERT_EQUAL_UINT32(1000, r.timestampMs);
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
}

void test_gps_sample_without_fix_is_not_valid_but_sample_succeeds() {
  FakeClock clock;
  FakeGpsDriver driver;

  driver.data.fix = false;
  driver.data.satellites = 3;

  Pa1010dGpsSensor::Config cfg;
  cfg.dutyClass = SensorDutyClass::AlwaysOn;
  cfg.minSamplePeriodMs = 0;

  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sample());

  const auto &r = sensor.reading();

  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_FALSE(r.fix);
  TEST_ASSERT_EQUAL_UINT8(3, r.satellites);
}

void test_gps_read_failure_fails_sample() {
  FakeClock clock;
  FakeGpsDriver driver;
  driver.readOk = false;

  Pa1010dGpsSensor::Config cfg;
  cfg.dutyClass = SensorDutyClass::AlwaysOn;
  cfg.minSamplePeriodMs = 0;

  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_FALSE(sensor.reading().valid);
}

void test_gps_duty_cycled_wake_delay_then_sample() {
  FakeClock clock;
  FakeGpsDriver driver;

  driver.data.fix = true;
  driver.data.satellites = 5;

  Pa1010dGpsSensor::Config cfg;
  cfg.dutyClass = SensorDutyClass::DutyCycled;
  cfg.wakeDelayMs = 25;
  cfg.minSamplePeriodMs = 100;

  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  assertState(sensor, SensorPowerState::Ready);

  TEST_ASSERT_TRUE(sensor.sleep());
  assertState(sensor, SensorPowerState::Sleeping);

  TEST_ASSERT_TRUE(sensor.wake());
  assertState(sensor, SensorPowerState::Waking);

  clock.advance(24);
  TEST_ASSERT_FALSE(sensor.service());

  clock.advance(1);
  TEST_ASSERT_TRUE(sensor.service());
  assertState(sensor, SensorPowerState::Ready);

  clock.advance(100);
  TEST_ASSERT_TRUE(sensor.sample());
  TEST_ASSERT_TRUE(sensor.reading().valid);
}

void test_gps_telemetry_writes_string() {
  FakeClock clock;
  FakeGpsDriver driver;

  driver.data.fix = true;
  driver.data.satellites = 9;
  driver.data.latitudeDeg = 46.1f;
  driver.data.longitudeDeg = -114.2f;
  driver.data.altitudeM = 1000.0f;
  driver.data.hour = 1;
  driver.data.minute = 2;
  driver.data.second = 3;

  Pa1010dGpsSensor::Config cfg;
  cfg.dutyClass = SensorDutyClass::AlwaysOn;
  cfg.minSamplePeriodMs = 0;

  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sample());

  char buf[180];
  const size_t n = sensor.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_NOT_NULL(strstr(buf, "gps"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "fix=1"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "sats=9"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "lat="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "lon="));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_gps_begin_success_starts_ready_for_always_on);
  RUN_TEST(test_gps_begin_failure_enters_error);
  RUN_TEST(test_gps_service_polls_driver);
  RUN_TEST(test_gps_sample_with_fix);
  RUN_TEST(test_gps_sample_without_fix_is_not_valid_but_sample_succeeds);
  RUN_TEST(test_gps_read_failure_fails_sample);
  RUN_TEST(test_gps_duty_cycled_wake_delay_then_sample);
  RUN_TEST(test_gps_telemetry_writes_string);

  return UNITY_END();
}
