#include <unity.h>
#include <cstring>

#include "app/SmartFiresNodeApp.h"

#include "fakes/FakeAnalogReader.h"
#include "fakes/FakeClock.h"
#include "fakes/FakeRadio.h"
#include "fakes/FakeSensor.h"

#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"
#include "radio/RadioService.h"
#include "telemetry/TelemetryBuilder.h"

static void assertStrContains(const char *s, const char *sub) {
  TEST_ASSERT_NOT_NULL(strstr(s, sub));
}
void test_app_full_cycle_sends_telemetry() {
  FakeClock clock;

  // --- Sensors ---
  FakeSensor sht31("sht31", SensorDutyClass::DutyCycled, clock);
  sht31.wakeDelayMs = 10;

  FakeSensor gps("gps", SensorDutyClass::AlwaysOn, clock);

  ISensor *sensors[] = {&sht31, &gps};

  // --- Battery ---
  FakeAnalogReader analog;
  analog.set(0, 620);

  BatteryMonitor::Config batCfg;
  batCfg.pin = 0;
  batCfg.minSamplePeriodMs = 0;

  BatteryMonitor battery(batCfg, analog, clock);

  // --- Duty ---
  DutyCycleConfig dcCfg;
  dcCfg.sleepMs = 1000;
  dcCfg.maxWakeMs = 5000;

  DutyCycleController duty(dcCfg, sensors, 2, clock);

  // --- Telemetry ---
  TelemetryBuilder::Config tbCfg;
  tbCfg.nodeId = 99;
  tbCfg.includeBattery = true;

  TelemetryBuilder telemetry(tbCfg);

  // --- Radio ---
  FakeRadio radio;

  RadioService::Config rCfg;
  rCfg.nodeId = 99;
  rCfg.requireAck = false;

  RadioService radioSvc(rCfg, radio, clock);

  // --- App ---
  SmartFiresNodeApp::Config appCfg;

  SmartFiresNodeApp app(appCfg,
                        clock,
                        duty,
                        telemetry,
                        radioSvc,
                        sensors,
                        2,
                        &battery);

  TEST_ASSERT_TRUE(app.begin());

  // --- Run loop ---
  clock.advance(1000);
  app.update(); // wake

  clock.advance(10);
  app.update(); // ready

  app.update(); // sample + send

  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);

  const char *sent = radio.lastSent();

  assertStrContains(sent, "node=99");
  assertStrContains(sent, "sht31");
  assertStrContains(sent, "gps");
  assertStrContains(sent, "battery");
}
void test_app_ack_flow() {
  FakeClock clock;

  FakeSensor sensor("sht31", SensorDutyClass::DutyCycled, clock);
  sensor.wakeDelayMs = 0;

  ISensor *sensors[] = {&sensor};

  DutyCycleConfig dcCfg;
  dcCfg.sleepMs = 0;
  dcCfg.maxWakeMs = 1000;

  DutyCycleController duty(dcCfg, sensors, 1, clock);

  TelemetryBuilder::Config tbCfg;
  TelemetryBuilder telemetry(tbCfg);

  FakeRadio radio;

  RadioService::Config rCfg;
  rCfg.requireAck = true;
  rCfg.ackTimeoutMs = 100;

  RadioService radioSvc(rCfg, radio, clock);

  SmartFiresNodeApp::Config appCfg;

  SmartFiresNodeApp app(appCfg,
                        clock,
                        duty,
                        telemetry,
                        radioSvc,
                        sensors,
                        1,
                        nullptr);

  TEST_ASSERT_TRUE(app.begin());

  // run cycle
  app.update();
  app.update();
  app.update();

  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);

  radio.queueRx("ACK,1");
  app.update();

  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(RadioServiceState::Ready),
      static_cast<uint8_t>(radioSvc.state()));
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_app_full_cycle_sends_telemetry);
  RUN_TEST(test_app_ack_flow);

  return UNITY_END();
}
