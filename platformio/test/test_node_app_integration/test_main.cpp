#include <unity.h>
#include <cstring>

#include "fakes/FakeAnalogReader.h"
#include "fakes/FakeClock.h"
#include "fakes/FakeRadio.h"
#include "fakes/FakeSensor.h"

#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"
#include "radio/RadioService.h"
#include "telemetry/TelemetryBuilder.h"

static void assertStrContains(const char *haystack, const char *needle) {
  TEST_ASSERT_NOT_NULL(haystack);
  TEST_ASSERT_NOT_NULL(needle);
  TEST_ASSERT_NOT_NULL(strstr(haystack, needle));
}
void test_full_node_cycle_sends_telemetry_and_gets_ack() {
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

  // --- Duty cycle ---
  DutyCycleConfig dcCfg;
  dcCfg.sleepMs = 1000;
  dcCfg.maxWakeMs = 5000;

  DutyCycleController duty(dcCfg, sensors, 2, clock);

  // --- Telemetry ---
  TelemetryBuilder::Config tbCfg;
  tbCfg.nodeId = 42;
  tbCfg.includeBattery = true;

  TelemetryBuilder telemetry(tbCfg);

  // --- Radio ---
  FakeRadio radio;

  RadioService::Config radioCfg;
  radioCfg.nodeId = 42;
  radioCfg.requireAck = true;
  radioCfg.ackTimeoutMs = 100;

  RadioService radioService(radioCfg, radio, clock);

  // --- Begin everything ---
  TEST_ASSERT_TRUE(battery.begin());
  TEST_ASSERT_TRUE(duty.begin());
  TEST_ASSERT_TRUE(radioService.begin());

  // --- Initial state ---
  TEST_ASSERT_FALSE(duty.telemetryReady());

  // --- Advance time to trigger wake ---
  clock.advance(1000);
  duty.update();

  // --- Sensors should wake ---
  TEST_ASSERT_TRUE(sht31.wakeCalled);
  TEST_ASSERT_FALSE(gps.wakeCalled);

  // --- Finish wake delay ---
  clock.advance(10);
  duty.update();

  // --- Sampling phase ---
  duty.update();

  TEST_ASSERT_TRUE(sht31.sampleCalled);
  TEST_ASSERT_TRUE(gps.sampleCalled);

  TEST_ASSERT_TRUE(duty.telemetryReady());

  // --- Build telemetry ---
  TelemetryFrame frame;

  TEST_ASSERT_TRUE(
      telemetry.build(frame, sensors, 2, &battery));

  assertStrContains(frame.payload, "node=42");
  assertStrContains(frame.payload, "sht31");
  assertStrContains(frame.payload, "gps");
  assertStrContains(frame.payload, "battery");

  // --- Send telemetry ---
  TEST_ASSERT_TRUE(radioService.sendTelemetry(frame));

  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);

  const char *sent = radio.lastSent();

  assertStrContains(sent, "T,node=42,seq=1");

  // --- Inject ACK ---
  radio.queueRx("ACK,1");
  radioService.update();

  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(RadioServiceState::Ready),
      static_cast<uint8_t>(radioService.state()));

  // --- Mark telemetry sent → back to sleep ---
  duty.markTelemetrySent();

  TEST_ASSERT_FALSE(duty.telemetryReady());
}

void test_retry_and_timeout_behavior() {
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

  RadioService::Config radioCfg;
  radioCfg.requireAck = true;
  radioCfg.ackTimeoutMs = 100;
  radioCfg.maxRetries = 2;

  RadioService service(radioCfg, radio, clock);

  TEST_ASSERT_TRUE(duty.begin());
  TEST_ASSERT_TRUE(service.begin());

  duty.update(); // wake
  duty.update(); // ready
  duty.update(); // sample

  TEST_ASSERT_TRUE(duty.telemetryReady());

  TelemetryFrame frame;
  TEST_ASSERT_TRUE(telemetry.build(frame, sensors, 1, nullptr));

  TEST_ASSERT_TRUE(service.sendTelemetry(frame));

  TEST_ASSERT_EQUAL_UINT32(1, radio.sendCount);

  // --- First timeout → retry ---
  clock.advance(100);
  service.update();

  TEST_ASSERT_EQUAL_UINT32(2, radio.sendCount);

  // --- Second timeout → retry ---
  clock.advance(100);
  service.update();

  TEST_ASSERT_EQUAL_UINT32(3, radio.sendCount);

  // --- Third timeout → error ---
  clock.advance(100);
  service.update();

  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(RadioServiceState::Error),
      static_cast<uint8_t>(service.state()));
}

void test_command_flow_through_radio() {
  FakeClock clock;
  FakeRadio radio;

  RadioService::Config cfg;
  RadioService service(cfg, radio, clock);

  TEST_ASSERT_TRUE(service.begin());

  radio.queueRx("CMD,set_interval=15000");
  service.update();

  TEST_ASSERT_TRUE(service.hasCommand());

  char buf[64];
  size_t n = service.readCommand(buf, sizeof(buf));

  TEST_ASSERT_GREATER_THAN_UINT32(0, n);
  TEST_ASSERT_EQUAL_STRING("set_interval=15000", buf);
}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_full_node_cycle_sends_telemetry_and_gets_ack);
  RUN_TEST(test_retry_and_timeout_behavior);
  RUN_TEST(test_command_flow_through_radio);

  return UNITY_END();
}
