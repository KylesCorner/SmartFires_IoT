#include <unity.h>
#include <cstring>

#include "fakes/FakeAnalogReader.h"
#include "fakes/FakeClock.h"
#include "fakes/FakeSensor.h"
#include "power/BatteryMonitor.h"
#include "telemetry/TelemetryBuilder.h"

void test_telemetry_builds_node_only() {
  TelemetryBuilder::Config cfg;
  cfg.nodeId = 7;
  cfg.includeBattery = false;

  TelemetryBuilder builder(cfg);

  TelemetryFrame frame;
  ISensor *sensors[] = {};

  TEST_ASSERT_TRUE(builder.build(frame, sensors, 0, nullptr));
  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "node=7"));
  TEST_ASSERT_GREATER_THAN_UINT32(0, frame.len);
}

void test_telemetry_builds_with_fake_sensor() {
  FakeClock clock;
  FakeSensor sht31("sht31", SensorDutyClass::DutyCycled, clock);

  TEST_ASSERT_TRUE(sht31.begin());
  TEST_ASSERT_TRUE(sht31.wake());
  TEST_ASSERT_TRUE(sht31.service());
  TEST_ASSERT_TRUE(sht31.sample());

  ISensor *sensors[] = {&sht31};

  TelemetryBuilder::Config cfg;
  cfg.nodeId = 2;
  cfg.includeBattery = false;

  TelemetryBuilder builder(cfg);
  TelemetryFrame frame;

  TEST_ASSERT_TRUE(builder.build(frame, sensors, 1, nullptr));

  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "node=2"));
  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "sht31"));
  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "samples=1"));
}

void test_telemetry_builds_with_battery_and_sensor() {
  FakeClock clock;
  FakeAnalogReader analog;

  BatteryMonitor::Config batteryCfg;
  batteryCfg.pin = 0;
  batteryCfg.minSamplePeriodMs = 0;

  analog.set(0, 620);

  BatteryMonitor battery(batteryCfg, analog, clock);

  TEST_ASSERT_TRUE(battery.begin());
  TEST_ASSERT_TRUE(battery.sample());

  FakeSensor gps("gps", SensorDutyClass::AlwaysOn, clock);

  TEST_ASSERT_TRUE(gps.begin());
  TEST_ASSERT_TRUE(gps.sample());

  ISensor *sensors[] = {&gps};

  TelemetryBuilder::Config cfg;
  cfg.nodeId = 3;
  cfg.includeBattery = true;

  TelemetryBuilder builder(cfg);
  TelemetryFrame frame;

  TEST_ASSERT_TRUE(builder.build(frame, sensors, 1, &battery));

  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "node=3"));
  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "battery"));
  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "gps"));
}

void test_telemetry_skips_null_sensor() {
  TelemetryBuilder::Config cfg;
  cfg.nodeId = 1;
  cfg.includeBattery = false;

  TelemetryBuilder builder(cfg);
  TelemetryFrame frame;

  ISensor *sensors[] = {nullptr};

  TEST_ASSERT_TRUE(builder.build(frame, sensors, 1, nullptr));
  TEST_ASSERT_NOT_NULL(strstr(frame.payload, "node=1"));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_telemetry_builds_node_only);
  RUN_TEST(test_telemetry_builds_with_fake_sensor);
  RUN_TEST(test_telemetry_builds_with_battery_and_sensor);
  RUN_TEST(test_telemetry_skips_null_sensor);

  return UNITY_END();
}
