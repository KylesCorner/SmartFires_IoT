#include <unity.h>

#include "fakes/FakeClock.h"
#include "fakes/FakeSensor.h"
#include "power/DutyCycleController.h"

static void assertPhase(DutyCycleController &controller,
                        DutyCyclePhase expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(controller.phase()));
}

static void assertError(DutyCycleController &controller,
                        DutyCycleError expected) {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expected),
                          static_cast<uint8_t>(controller.error()));
}

void test_begin_starts_sensors_and_enters_sleeping() {
  FakeClock clock;

  FakeSensor sht31("sht31", SensorDutyClass::DutyCycled, clock);
  FakeSensor gps("gps", SensorDutyClass::AlwaysOn, clock);

  ISensor *sensors[] = {&sht31, &gps};

  DutyCycleConfig cfg;
  DutyCycleController controller(cfg, sensors, 2, clock);

  TEST_ASSERT_TRUE(controller.begin());

  TEST_ASSERT_TRUE(sht31.beginCalled);
  TEST_ASSERT_TRUE(gps.beginCalled);

  TEST_ASSERT_TRUE(sht31.sleepCalled);
  TEST_ASSERT_FALSE(gps.sleepCalled);

  assertPhase(controller, DutyCyclePhase::Sleeping);
  assertError(controller, DutyCycleError::None);
}

void test_full_cycle_wake_sample_telemetry_sleep() {
  FakeClock clock;

  FakeSensor sht31("sht31", SensorDutyClass::DutyCycled, clock);
  sht31.wakeDelayMs = 15;

  FakeSensor gps("gps", SensorDutyClass::AlwaysOn, clock);

  ISensor *sensors[] = {&sht31, &gps};

  DutyCycleConfig cfg;
  cfg.sleepMs = 1000;
  cfg.maxWakeMs = 5000;

  DutyCycleController controller(cfg, sensors, 2, clock);

  TEST_ASSERT_TRUE(controller.begin());

  clock.advance(999);
  controller.update();
  assertPhase(controller, DutyCyclePhase::Sleeping);
  TEST_ASSERT_FALSE(sht31.wakeCalled);

  clock.advance(1);
  controller.update();
  assertPhase(controller, DutyCyclePhase::WakingSensors);
  TEST_ASSERT_TRUE(sht31.wakeCalled);
  TEST_ASSERT_FALSE(gps.wakeCalled);

  clock.advance(14);
  controller.update();
  assertPhase(controller, DutyCyclePhase::WakingSensors);

  clock.advance(1);
  controller.update();
  assertPhase(controller, DutyCyclePhase::Sampling);

  controller.update();
  assertPhase(controller, DutyCyclePhase::TelemetryReady);

  TEST_ASSERT_TRUE(sht31.sampleCalled);
  TEST_ASSERT_TRUE(gps.sampleCalled);
  TEST_ASSERT_TRUE(controller.telemetryReady());

  controller.markTelemetrySent();

  assertPhase(controller, DutyCyclePhase::Sleeping);
  TEST_ASSERT_TRUE(sht31.sleepCalled);
}

void test_begin_failure_enters_error() {
  FakeClock clock;

  FakeSensor badSensor("bad", SensorDutyClass::DutyCycled, clock);
  badSensor.beginShouldPass = false;

  ISensor *sensors[] = {&badSensor};

  DutyCycleConfig cfg;
  DutyCycleController controller(cfg, sensors, 1, clock);

  TEST_ASSERT_FALSE(controller.begin());
  assertPhase(controller, DutyCyclePhase::Error);
  assertError(controller, DutyCycleError::SensorBeginFailed);
}

void test_wake_timeout_enters_error() {
  FakeClock clock;

  FakeSensor slowSensor("slow", SensorDutyClass::DutyCycled, clock);
  slowSensor.wakeDelayMs = 10000;

  ISensor *sensors[] = {&slowSensor};

  DutyCycleConfig cfg;
  cfg.sleepMs = 1000;
  cfg.maxWakeMs = 500;

  DutyCycleController controller(cfg, sensors, 1, clock);

  TEST_ASSERT_TRUE(controller.begin());

  clock.advance(1000);
  controller.update();
  assertPhase(controller, DutyCyclePhase::WakingSensors);

  clock.advance(500);
  controller.update();

  assertPhase(controller, DutyCyclePhase::Error);
  assertError(controller, DutyCycleError::SensorWakeTimeout);
}

void test_sample_failure_can_be_nonfatal() {
  FakeClock clock;

  FakeSensor sensor("sht31", SensorDutyClass::DutyCycled, clock);
  sensor.sampleShouldPass = false;

  ISensor *sensors[] = {&sensor};

  DutyCycleConfig cfg;
  cfg.sleepMs = 0;
  cfg.maxWakeMs = 1000;
  cfg.failOnSampleError = false;

  DutyCycleController controller(cfg, sensors, 1, clock);

  TEST_ASSERT_TRUE(controller.begin());

  controller.update();
  controller.update();
  controller.update();

  assertPhase(controller, DutyCyclePhase::TelemetryReady);
  assertError(controller, DutyCycleError::None);
}

void test_sample_failure_can_be_fatal() {
  FakeClock clock;

  FakeSensor sensor("sht31", SensorDutyClass::DutyCycled, clock);
  sensor.sampleShouldPass = false;

  ISensor *sensors[] = {&sensor};

  DutyCycleConfig cfg;
  cfg.sleepMs = 0;
  cfg.maxWakeMs = 1000;
  cfg.failOnSampleError = true;

  DutyCycleController controller(cfg, sensors, 1, clock);

  TEST_ASSERT_TRUE(controller.begin());

  controller.update();
  controller.update();
  controller.update();

  assertPhase(controller, DutyCyclePhase::Error);
  assertError(controller, DutyCycleError::SensorSampleFailed);
}

// void setup() {
//   UNITY_BEGIN();
//
//   RUN_TEST(test_begin_starts_sensors_and_enters_sleeping);
//   RUN_TEST(test_full_cycle_wake_sample_telemetry_sleep);
//   RUN_TEST(test_begin_failure_enters_error);
//   RUN_TEST(test_wake_timeout_enters_error);
//   RUN_TEST(test_sample_failure_can_be_nonfatal);
//   RUN_TEST(test_sample_failure_can_be_fatal);
//
//   UNITY_END();
// }
//
// void loop() {}
int main() {
  UNITY_BEGIN();

  RUN_TEST(test_begin_starts_sensors_and_enters_sleeping);
  RUN_TEST(test_full_cycle_wake_sample_telemetry_sleep);
  RUN_TEST(test_begin_failure_enters_error);
  RUN_TEST(test_wake_timeout_enters_error);
  RUN_TEST(test_sample_failure_can_be_nonfatal);
  RUN_TEST(test_sample_failure_can_be_fatal);

  return UNITY_END();
}
