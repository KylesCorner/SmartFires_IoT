#include <cstring>
#include <unity.h>

#include "app/SmartFiresNodeApp.h"

#include "fakes/FakeAnalogReader.h"
#include "fakes/FakeClock.h"
#include "fakes/FakeSensor.h"
#include "fakes/FakeTdmaRadioDriver.h"

#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"

#include "radio/TdmaClock.h"
#include "radio/TdmaConfig.h"
#include "radio/TdmaRadioService.h"
#include "radio/TdmaTxQueue.h"

#include "telemetry/TelemetryBuilder.h"

static void assertStrContains(const char *s, const char *sub) {
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_NOT_NULL(sub);
  TEST_ASSERT_NOT_NULL(strstr(s, sub));
}

void test_app_full_cycle_sends_telemetry() {
  FakeClock clock;

  FakeSensor sht31("sht31", SensorDutyClass::DutyCycled, clock);
  sht31.wakeDelayMs = 10;

  FakeSensor gps("gps", SensorDutyClass::AlwaysOn, clock);

  ISensor *sensors[] = {&sht31, &gps};

  FakeAnalogReader analog;
  analog.set(0, 620);

  BatteryMonitor::Config batCfg;
  batCfg.pin = 0;
  batCfg.minSamplePeriodMs = 0;

  BatteryMonitor battery(batCfg, analog, clock);

  DutyCycleConfig dcCfg;
  dcCfg.sleepMs = 1000;
  dcCfg.maxWakeMs = 5000;

  DutyCycleController duty(dcCfg, sensors, 2, clock);

  TelemetryBuilder::Config tbCfg;
  tbCfg.nodeId = 99;
  tbCfg.includeBattery = true;

  TelemetryBuilder telemetry(tbCfg);

  TdmaConfig tdmaCfg;
  tdmaCfg.nodeId = 99;
  tdmaCfg.baseAddr = 0x01;
  tdmaCfg.numSlots = 2;
  tdmaCfg.slotWidthMs = 900;
  tdmaCfg.guardMs = 20;

  TdmaClock tdmaClock(tdmaCfg, clock);
  TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);
  FakeTdmaRadioDriver radioDriver;

  TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

  SmartFiresNodeApp::Config appCfg;
  appCfg.enableBattery = true;

  SmartFiresNodeApp app(appCfg, clock, duty, telemetry, tdmaRadio, sensors, 2,
                        &battery);

  TEST_ASSERT_TRUE(app.begin());

  clock.advance(1000);
  app.update(); // duty wakes SHT31

  TEST_ASSERT_TRUE(sht31.wakeCalled);
  TEST_ASSERT_FALSE(gps.wakeCalled);

  clock.advance(10);
  app.update(); // SHT31 becomes ready

  app.update(); // duty samples, app builds telemetry, queues TDMA packet

  TEST_ASSERT_TRUE(sht31.sampleCalled);
  TEST_ASSERT_TRUE(gps.sampleCalled);

  TEST_ASSERT_EQUAL_UINT32(0, radioDriver.sendCount);
  TEST_ASSERT_EQUAL_UINT8(1, tdmaRadio.queuedCount());

  app.update(); // TDMA drains queued packet

  TEST_ASSERT_EQUAL_UINT32(1, radioDriver.sendCount);

  const char *sent = reinterpret_cast<const char *>(radioDriver.lastSent);

  assertStrContains(sent, "node=99");
  assertStrContains(sent, "sht31");
  assertStrContains(sent, "gps");
  assertStrContains(sent, "battery");
}

void test_app_tdma_flow_sends_when_slot_available() {
  FakeClock clock;

  FakeSensor sensor("sht31", SensorDutyClass::DutyCycled, clock);
  sensor.wakeDelayMs = 0;

  ISensor *sensors[] = {&sensor};

  DutyCycleConfig dcCfg;
  dcCfg.sleepMs = 0;
  dcCfg.maxWakeMs = 1000;

  DutyCycleController duty(dcCfg, sensors, 1, clock);

  TelemetryBuilder::Config tbCfg;
  tbCfg.nodeId = 1;
  tbCfg.includeBattery = false;

  TelemetryBuilder telemetry(tbCfg);

  TdmaConfig tdmaCfg;
  tdmaCfg.nodeId = 1;
  tdmaCfg.baseAddr = 0x01;
  tdmaCfg.numSlots = 2;
  tdmaCfg.slotWidthMs = 900;
  tdmaCfg.guardMs = 20;

  TdmaClock tdmaClock(tdmaCfg, clock);
  TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);
  FakeTdmaRadioDriver radioDriver;

  TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

  SmartFiresNodeApp::Config appCfg;
  appCfg.enableBattery = false;

  SmartFiresNodeApp app(appCfg, clock, duty, telemetry, tdmaRadio, sensors, 1,
                        nullptr);

  TEST_ASSERT_TRUE(app.begin());

  // app.update(); // wake
  // app.update(); // ready
  // app.update(); // sample + queue/send
  //
  // TEST_ASSERT_TRUE(sensor.sampleCalled);
  // TEST_ASSERT_EQUAL_UINT32(1, radioDriver.sendCount);
  app.update(); // wake
  app.update(); // ready
  app.update(); // sample + queue

  TEST_ASSERT_TRUE(sensor.sampleCalled);
  TEST_ASSERT_EQUAL_UINT32(0, radioDriver.sendCount);
  TEST_ASSERT_EQUAL_UINT8(1, tdmaRadio.queuedCount());

  app.update(); // TDMA drains queued packet

  TEST_ASSERT_EQUAL_UINT32(1, radioDriver.sendCount);

  const char *sent = reinterpret_cast<const char *>(radioDriver.lastSent);

  assertStrContains(sent, "node=1");
  assertStrContains(sent, "sht31");
}

void test_app_tdma_waits_for_correct_slot_when_synced() {
  FakeClock clock;

  FakeSensor sensor("sht31", SensorDutyClass::DutyCycled, clock);
  sensor.wakeDelayMs = 0;

  ISensor *sensors[] = {&sensor};

  DutyCycleConfig dcCfg;
  dcCfg.sleepMs = 0;
  dcCfg.maxWakeMs = 1000;

  DutyCycleController duty(dcCfg, sensors, 1, clock);

  TelemetryBuilder::Config tbCfg;
  tbCfg.nodeId = 2;
  tbCfg.includeBattery = false;

  TelemetryBuilder telemetry(tbCfg);

  TdmaConfig tdmaCfg;
  tdmaCfg.nodeId = 2; // node 2 owns slot 1
  tdmaCfg.baseAddr = 0x01;
  tdmaCfg.numSlots = 2;
  tdmaCfg.slotWidthMs = 900;
  tdmaCfg.guardMs = 20;

  TdmaClock tdmaClock(tdmaCfg, clock);
  TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);
  FakeTdmaRadioDriver radioDriver;

  TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

  SmartFiresNodeApp::Config appCfg;
  appCfg.enableBattery = false;

  SmartFiresNodeApp app(appCfg, clock, duty, telemetry, tdmaRadio, sensors, 1,
                        nullptr);

  TEST_ASSERT_TRUE(app.begin());

  tdmaClock.applySync(0);

  // At t=0, node 2 is not in its slot.
  app.update(); // wake
  app.update(); // ready
  app.update(); // sample + queue, but TDMA should not send yet

  TEST_ASSERT_TRUE(sensor.sampleCalled);
  TEST_ASSERT_EQUAL_UINT32(0, radioDriver.sendCount);
  TEST_ASSERT_EQUAL_UINT8(1, tdmaRadio.queuedCount());

  // Move into node 2's slot, after leading guard.
  clock.set(925);
  app.update();

  TEST_ASSERT_EQUAL_UINT32(1, radioDriver.sendCount);
  TEST_ASSERT_EQUAL_UINT8(0, tdmaRadio.queuedCount());
}

void test_app_does_not_run_before_begin() {
  FakeClock clock;

  FakeSensor sensor("sht31", SensorDutyClass::DutyCycled, clock);
  ISensor *sensors[] = {&sensor};

  DutyCycleConfig dcCfg;
  dcCfg.sleepMs = 0;
  dcCfg.maxWakeMs = 1000;

  DutyCycleController duty(dcCfg, sensors, 1, clock);

  TelemetryBuilder::Config tbCfg;
  TelemetryBuilder telemetry(tbCfg);

  TdmaConfig tdmaCfg;
  TdmaClock tdmaClock(tdmaCfg, clock);
  TdmaTxQueue tdmaQueue(tdmaCfg.queueDepth);
  FakeTdmaRadioDriver radioDriver;

  TdmaRadioService tdmaRadio(tdmaCfg, tdmaClock, tdmaQueue, radioDriver);

  SmartFiresNodeApp::Config appCfg;
  appCfg.enableBattery = false;

  SmartFiresNodeApp app(appCfg, clock, duty, telemetry, tdmaRadio, sensors, 1,
                        nullptr);

  app.update();

  TEST_ASSERT_FALSE(sensor.beginCalled);
  TEST_ASSERT_EQUAL_UINT32(0, radioDriver.sendCount);
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_app_full_cycle_sends_telemetry);
  RUN_TEST(test_app_tdma_flow_sends_when_slot_available);
  RUN_TEST(test_app_tdma_waits_for_correct_slot_when_synced);
  RUN_TEST(test_app_does_not_run_before_begin);

  return UNITY_END();
}
