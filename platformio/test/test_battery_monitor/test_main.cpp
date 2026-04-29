#include <unity.h>
#include <cstring>

#include "fakes/FakeAnalogReader.h"
#include "fakes/FakeClock.h"
#include "power/BatteryMonitor.h"

void test_battery_begin_success() {
  FakeClock clock;
  FakeAnalogReader analog;

  BatteryMonitor::Config cfg;
  BatteryMonitor battery(cfg, analog, clock);

  TEST_ASSERT_TRUE(battery.begin());
  TEST_ASSERT_TRUE(battery.healthy());
}

void test_battery_sample_voltage_and_percent() {
  FakeClock clock;
  FakeAnalogReader analog;

  BatteryMonitor::Config cfg;
  cfg.pin = 0;
  cfg.adcRefVolts = 3.3f;
  cfg.adcMax = 1023;
  cfg.dividerRatio = 2.0f;
  cfg.minVoltage = 3.2f;
  cfg.maxVoltage = 4.2f;
  cfg.lowVoltage = 3.5f;
  cfg.minSamplePeriodMs = 1000;

  analog.set(0, 620);

  BatteryMonitor battery(cfg, analog, clock);

  TEST_ASSERT_TRUE(battery.begin());

  TEST_ASSERT_FALSE(battery.ready());

  clock.advance(1000);

  TEST_ASSERT_TRUE(battery.ready());
  TEST_ASSERT_TRUE(battery.sample());

  const auto &r = battery.reading();

  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_INT(620, r.raw);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.00f, r.adcVolts);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 4.00f, r.batteryVolts);
  TEST_ASSERT_FLOAT_WITHIN(2.0f, 80.0f, r.percent);
  TEST_ASSERT_FALSE(r.low);
  TEST_ASSERT_EQUAL_UINT32(1000, r.timestampMs);
}

void test_battery_low_voltage() {
  FakeClock clock;
  FakeAnalogReader analog;

  BatteryMonitor::Config cfg;
  cfg.pin = 0;
  cfg.adcRefVolts = 3.3f;
  cfg.adcMax = 1023;
  cfg.dividerRatio = 2.0f;
  cfg.minVoltage = 3.2f;
  cfg.maxVoltage = 4.2f;
  cfg.lowVoltage = 3.5f;
  cfg.minSamplePeriodMs = 0;

  analog.set(0, 527); // about 3.4V battery with dividerRatio 2.0

  BatteryMonitor battery(cfg, analog, clock);

  TEST_ASSERT_TRUE(battery.begin());
  TEST_ASSERT_TRUE(battery.sample());

  const auto &r = battery.reading();

  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_TRUE(r.low);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 3.40f, r.batteryVolts);
}

void test_battery_invalid_adc_fails() {
  FakeClock clock;
  FakeAnalogReader analog;

  BatteryMonitor::Config cfg;
  cfg.pin = 0;
  cfg.adcMax = 1023;
  cfg.minSamplePeriodMs = 0;

  analog.set(0, 2000);

  BatteryMonitor battery(cfg, analog, clock);

  TEST_ASSERT_TRUE(battery.begin());
  TEST_ASSERT_FALSE(battery.sample());
  TEST_ASSERT_FALSE(battery.healthy());
  TEST_ASSERT_FALSE(battery.reading().valid);
}

void test_battery_telemetry_writes_string() {
  FakeClock clock;
  FakeAnalogReader analog;

  BatteryMonitor::Config cfg;
  cfg.pin = 0;
  cfg.minSamplePeriodMs = 0;

  analog.set(0, 620);

  BatteryMonitor battery(cfg, analog, clock);

  TEST_ASSERT_TRUE(battery.begin());
  TEST_ASSERT_TRUE(battery.sample());

  char buf[128];
  size_t n = battery.writeTelemetry(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_NOT_NULL(strstr(buf, "battery"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "v="));
  TEST_ASSERT_NOT_NULL(strstr(buf, "pct="));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_battery_begin_success);
  RUN_TEST(test_battery_sample_voltage_and_percent);
  RUN_TEST(test_battery_low_voltage);
  RUN_TEST(test_battery_invalid_adc_fails);
  RUN_TEST(test_battery_telemetry_writes_string);

  return UNITY_END();
}
