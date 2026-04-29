#include <unity.h>
#include <cstring>

#include "fakes/FakeAnalogReader.h"
#include "fakes/FakeClock.h"
#include "sensors/WindSensorRevC.h"

static void assertState(WindSensorRevC &sensor, SensorPowerState expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(sensor.powerState()));
}

void test_wind_begin_starts_sleeping_for_duty_cycled() {
  FakeClock clock;
  FakeAnalogReader analog;

  WindSensorRevC::Config cfg;
  cfg.dutyClass = SensorDutyClass::DutyCycled;

  WindSensorRevC sensor(cfg, analog, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.healthy());
  assertState(sensor, SensorPowerState::Sleeping);
}

void test_wind_wake_service_sample() {
  FakeClock clock;
  FakeAnalogReader analog;

  WindSensorRevC::Config cfg;
  cfg.pinRv = 0;
  cfg.pinTmp = 1;
  cfg.adcRefVolts = 3.3f;
  cfg.adcMax = 1023;
  cfg.minSamplePeriodMs = 1000;
  cfg.wakeDelayMs = 100;
  cfg.zeroWindAdjustmentVolts = 0.2f;

  analog.set(0, 512);
  analog.set(1, 310);

  WindSensorRevC sensor(cfg, analog, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());

  clock.advance(99);
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
  TEST_ASSERT_EQUAL_INT(512, r.rawRv);
  TEST_ASSERT_EQUAL_INT(310, r.rawTmp);

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.65f, r.rvVolts);
  TEST_ASSERT_GREATER_THAN_FLOAT(0.0f, r.windMps);

  TEST_ASSERT_EQUAL_UINT32(1, analog.count(0));
  TEST_ASSERT_EQUAL_UINT32(1, analog.count(1));
}

void test_wind_invalid_adc_fails_sample() {
  FakeClock clock;
  FakeAnalogReader analog;

  WindSensorRevC::Config cfg;
  cfg.pinRv = 0;
  cfg.pinTmp = 1;
  cfg.minSamplePeriodMs = 0;

  analog.set(0, 2000);
  analog.set(1, 100);

  WindSensorRevC sensor(cfg, analog, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());

  TEST_ASSERT_FALSE(sensor.sample());

  const auto &r = sensor.reading();
  TEST_ASSERT_FALSE(r.valid);
}

void test_wind_sleep_after_wake() {
  FakeClock clock;
  FakeAnalogReader analog;

  WindSensorRevC::Config cfg;

  WindSensorRevC sensor(cfg, analog, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.sleep());

  assertState(sensor, SensorPowerState::Sleeping);
}

void test_wind_telemetry_writes_string() {
  FakeClock clock;
  FakeAnalogReader analog;

  WindSensorRevC::Config cfg;
  cfg.pinRv = 0;
  cfg.pinTmp = 1;
  cfg.minSamplePeriodMs = 0;

  analog.set(0, 512);
  analog.set(1, 310);

  WindSensorRevC sensor(cfg, analog, clock);

  TEST_ASSERT_TRUE(sensor.begin());
  TEST_ASSERT_TRUE(sensor.wake());
  TEST_ASSERT_TRUE(sensor.service());
  TEST_ASSERT_TRUE(sensor.sample());

  char buf[160];
  const size_t n = sensor.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_NOT_NULL(strstr(buf, "wind"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "rv_v="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "wind_mps="));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_wind_begin_starts_sleeping_for_duty_cycled);
  RUN_TEST(test_wind_wake_service_sample);
  RUN_TEST(test_wind_invalid_adc_fails_sample);
  RUN_TEST(test_wind_sleep_after_wake);
  RUN_TEST(test_wind_telemetry_writes_string);

  return UNITY_END();
}
