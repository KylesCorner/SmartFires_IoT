// test/test_gps_sensor/test_main.cpp

#include <unity.h>

#include "sensors/Pa1010dGpsSensor.h"

#include "fakes/FakeClock.h"
#include "fakes/FakeGpsDriver.h"

#include <cstring>

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

static void test_begin_success_sets_ready_and_uses_configured_address() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg(100, 0, 0x10);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
  TEST_ASSERT_EQUAL_UINT8(0x10, driver.lastAddress);
  TEST_ASSERT_EQUAL_UINT32(1, driver.beginCount);
}

static void test_begin_failure_sets_error_and_unhealthy() {
  FakeGpsDriver driver;
  FakeClock clock;
  driver.beginOk = false;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_FALSE(sensor.begin());

  TEST_ASSERT_FALSE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Error, sensor.powerState());
  TEST_ASSERT_EQUAL_UINT32(1, driver.beginCount);
}

static void test_name_is_gps() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_EQUAL_STRING("gps", sensor.name());
}

static void test_duty_class_matches_config() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeDutyCycledCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_EQUAL(SensorDutyClass::DutyCycled, sensor.dutyClass());
}

static void test_always_on_sleep_keeps_sensor_ready() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

static void test_always_on_wake_keeps_sensor_ready() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

static void test_duty_cycled_sleep_sets_sleeping() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeDutyCycledCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sleep());

  TEST_ASSERT_EQUAL(SensorPowerState::Sleeping, sensor.powerState());
}

static void test_duty_cycled_wake_sets_waking() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeDutyCycledCfg(100, 50);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sleep());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.wake());

  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
}

static void test_service_polls_driver() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_EQUAL_UINT32(1, driver.pollCount);
}

static void test_service_returns_false_if_poll_fails() {
  FakeGpsDriver driver;
  FakeClock clock;
  driver.pollOk = false;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  TEST_ASSERT_FALSE(sensor.service());

  TEST_ASSERT_EQUAL_UINT32(1, driver.pollCount);
  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

static void test_duty_cycled_service_does_not_become_ready_before_wake_delay() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeDutyCycledCfg(100, 50);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sleep());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.set(1049);
  TEST_ASSERT_FALSE(sensor.service());

  TEST_ASSERT_EQUAL(SensorPowerState::Waking, sensor.powerState());
  TEST_ASSERT_EQUAL_UINT32(1, driver.pollCount);
}

static void test_duty_cycled_service_becomes_ready_after_wake_delay() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeDutyCycledCfg(100, 50);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.sleep());

  clock.set(1000);
  TEST_ASSERT_TRUE(sensor.wake());

  clock.set(1050);
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
  TEST_ASSERT_EQUAL_UINT32(1, driver.pollCount);
}

static void test_ready_respects_min_sample_period() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(99);
  TEST_ASSERT_FALSE(sensor.ready());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.ready());
}

static void test_sample_returns_false_when_not_ready() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(99);
  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_EQUAL_UINT32(0, driver.readCount);
}

static void test_sample_success_with_fix_copies_reading_and_sets_valid() {
  FakeGpsDriver driver;
  FakeClock clock;

  driver.setFix(
      46.872100f,
      -113.994000f,
      978.5f,
      8,
      1,
      12,
      34,
      56);

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.sample());

  const Pa1010dGpsSensor::Reading &r = sensor.reading();

  TEST_ASSERT_TRUE(r.fix);
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT8(1, r.fixQuality);
  TEST_ASSERT_EQUAL_UINT8(8, r.satellites);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 46.872100f, r.latitudeDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -113.994000f, r.longitudeDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 978.5f, r.altitudeM);
  TEST_ASSERT_EQUAL_UINT8(12, r.hour);
  TEST_ASSERT_EQUAL_UINT8(34, r.minute);
  TEST_ASSERT_EQUAL_UINT8(56, r.second);
  TEST_ASSERT_EQUAL_UINT32(100, r.timestampMs);

  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);
}

static void test_sample_success_without_fix_sets_valid_false() {
  FakeGpsDriver driver;
  FakeClock clock;

  driver.setNoFix(1, 2, 3);

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.sample());

  const Pa1010dGpsSensor::Reading &r = sensor.reading();

  TEST_ASSERT_FALSE(r.fix);
  TEST_ASSERT_FALSE(r.valid);
  TEST_ASSERT_EQUAL_UINT8(0, r.fixQuality);
  TEST_ASSERT_EQUAL_UINT8(0, r.satellites);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, r.latitudeDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, r.longitudeDeg);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, r.altitudeM);
  TEST_ASSERT_EQUAL_UINT8(1, r.hour);
  TEST_ASSERT_EQUAL_UINT8(2, r.minute);
  TEST_ASSERT_EQUAL_UINT8(3, r.second);
  TEST_ASSERT_EQUAL_UINT32(100, r.timestampMs);
}

static void test_sample_read_failure_returns_false_and_marks_reading_invalid() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  driver.setFix(46.0f, -114.0f, 900.0f);
  clock.set(100);
  TEST_ASSERT_TRUE(sensor.sample());
  TEST_ASSERT_TRUE(sensor.reading().valid);

  driver.readOk = false;
  clock.set(200);
  TEST_ASSERT_FALSE(sensor.sample());

  TEST_ASSERT_FALSE(sensor.reading().valid);
  TEST_ASSERT_TRUE(sensor.healthy());
  TEST_ASSERT_EQUAL(SensorPowerState::Ready, sensor.powerState());
}

static void test_sample_rate_limiting_after_successful_sample() {
  FakeGpsDriver driver;
  FakeClock clock;

  driver.setFix(46.0f, -114.0f, 900.0f);

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.sample());
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);

  clock.set(199);
  TEST_ASSERT_FALSE(sensor.ready());
  TEST_ASSERT_FALSE(sensor.sample());
  TEST_ASSERT_EQUAL_UINT32(1, driver.readCount);

  clock.set(200);
  TEST_ASSERT_TRUE(sensor.ready());
  TEST_ASSERT_TRUE(sensor.sample());
  TEST_ASSERT_EQUAL_UINT32(2, driver.readCount);
}

static void test_reading_data_and_size() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_EQUAL_PTR(&sensor.reading(), sensor.readingData());
  TEST_ASSERT_EQUAL_UINT(sizeof(Pa1010dGpsSensor::Reading), sensor.readingSize());
}

static void test_write_telemetry_formats_current_reading() {
  FakeGpsDriver driver;
  FakeClock clock;

  driver.setFix(
      46.872100f,
      -113.994000f,
      978.5f,
      8,
      1,
      12,
      34,
      56);

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.sample());

  char out[192];
  size_t n = sensor.writeTelemetry(out, sizeof(out));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);

  TEST_ASSERT_EQUAL_STRING(
      "gps,fix=1,fixq=1,sats=8,lat=46.872101,lon=-113.994003,alt=978.50,t=12:34:56,valid=1,t_ms=100",
      out);
}

static void test_write_telemetry_handles_null_buffer() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_EQUAL_UINT(0, sensor.writeTelemetry(nullptr, 32));
}

static void test_write_telemetry_handles_zero_length_buffer() {
  FakeGpsDriver driver;
  FakeClock clock;

  auto cfg = makeAlwaysOnCfg();
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  char out[8];
  TEST_ASSERT_EQUAL_UINT(0, sensor.writeTelemetry(out, 0));
}

static void test_write_telemetry_truncates_safely() {
  FakeGpsDriver driver;
  FakeClock clock;

  driver.setFix(46.872100f, -113.994000f, 978.5f);

  auto cfg = makeAlwaysOnCfg(100);
  Pa1010dGpsSensor sensor(cfg, driver, clock);

  TEST_ASSERT_TRUE(sensor.begin());

  clock.set(100);
  TEST_ASSERT_TRUE(sensor.sample());

  char out[12];
  std::memset(out, 'X', sizeof(out));

  size_t n = sensor.writeTelemetry(out, sizeof(out));

  TEST_ASSERT_EQUAL_UINT(sizeof(out) - 1, n);
  TEST_ASSERT_EQUAL_CHAR('\0', out[sizeof(out) - 1]);
}

void runGpsSensorTests() {
  UNITY_BEGIN();

  RUN_TEST(test_begin_success_sets_ready_and_uses_configured_address);
  RUN_TEST(test_begin_failure_sets_error_and_unhealthy);
  RUN_TEST(test_name_is_gps);
  RUN_TEST(test_duty_class_matches_config);

  RUN_TEST(test_always_on_sleep_keeps_sensor_ready);
  RUN_TEST(test_always_on_wake_keeps_sensor_ready);
  RUN_TEST(test_duty_cycled_sleep_sets_sleeping);
  RUN_TEST(test_duty_cycled_wake_sets_waking);

  RUN_TEST(test_service_polls_driver);
  RUN_TEST(test_service_returns_false_if_poll_fails);
  RUN_TEST(test_duty_cycled_service_does_not_become_ready_before_wake_delay);
  RUN_TEST(test_duty_cycled_service_becomes_ready_after_wake_delay);

  RUN_TEST(test_ready_respects_min_sample_period);
  RUN_TEST(test_sample_returns_false_when_not_ready);
  RUN_TEST(test_sample_success_with_fix_copies_reading_and_sets_valid);
  RUN_TEST(test_sample_success_without_fix_sets_valid_false);
  RUN_TEST(test_sample_read_failure_returns_false_and_marks_reading_invalid);
  RUN_TEST(test_sample_rate_limiting_after_successful_sample);

  RUN_TEST(test_reading_data_and_size);
  RUN_TEST(test_write_telemetry_formats_current_reading);
  RUN_TEST(test_write_telemetry_handles_null_buffer);
  RUN_TEST(test_write_telemetry_handles_zero_length_buffer);
  RUN_TEST(test_write_telemetry_truncates_safely);

  UNITY_END();
}

#ifdef ARDUINO
void setup() {
  delay(2000);
  runGpsSensorTests();
}

void loop() {}
#else
int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  runGpsSensorTests();
  return 0;
}
#endif
