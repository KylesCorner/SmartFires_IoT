#include <unity.h>

#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"

#include "../support/fakes/FakeAnalogReader.h"
#include "../support/fakes/FakeClock.h"
#include "../support/fakes/FakeSensor.h"
#include "../support/fakes/FakeTriggerSensor.h"

// DutyCycleController samples the battery on every update(); these tests never
// assert on it, so one fake ADC behind a real BatteryMonitor is enough to
// construct the controller.
struct TestBattery {
  FakeAnalogReader analog;
  BatteryMonitor monitor;

  explicit TestBattery(IClock &clock)
      : monitor(BatteryMonitor::Config::makeBatConfig(), analog, clock) {}
};

static DutyCycleConfig makeTestConfig() {
  DutyCycleConfig cfg;

  cfg.warmupMs = 100;
  cfg.samplePeriodMs = 10;
  cfg.activeSampleMs = 50;
  cfg.minSleepMs = 500;

  cfg.tempDeltaThresholdC = 1.0f;
  cfg.humidityDeltaThresholdPct = 5.0f;
  cfg.enabled = true;

  return cfg;
}

static void moveFromWarmingToActive(DutyCycleController &duty, FakeClock &clock,
                                    const DutyCycleConfig &cfg) {
  clock.advance(cfg.warmupMs);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());
}

static void moveFromActiveToCooldown(DutyCycleController &duty,
                                     FakeClock &clock,
                                     const DutyCycleConfig &cfg) {
  clock.advance(cfg.activeSampleMs);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());
}

static void moveFromCooldownToIdle(DutyCycleController &duty, FakeClock &clock,
                                   const DutyCycleConfig &cfg) {
  clock.advance(cfg.minSleepMs);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());
}

void test_begin_starts_warmup_after_begin_sleep_wake_sequence() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor sht31("sht31", SensorDutyClass::AlwaysOn);
  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  FakeSensor gps("gps", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &sht31,
      &imu,
      &gps,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 3, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());
  TEST_ASSERT_EQUAL(DutyCycleError::None, duty.error());

  TEST_ASSERT_EQUAL(1, sht31.beginCount);
  TEST_ASSERT_EQUAL(1, imu.beginCount);
  TEST_ASSERT_EQUAL(1, gps.beginCount);

  TEST_ASSERT_EQUAL(0, sht31.sleepCount);
  TEST_ASSERT_EQUAL(0, sht31.wakeCount);

  TEST_ASSERT_EQUAL(1, imu.sleepCount);
  TEST_ASSERT_EQUAL(1, gps.sleepCount);

  TEST_ASSERT_EQUAL(1, imu.wakeCount);
  TEST_ASSERT_EQUAL(1, gps.wakeCount);
}

void test_begin_enters_error_if_sensor_begin_fails() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.beginOk = false;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_FALSE(duty.begin());

  TEST_ASSERT_EQUAL(DutyCyclePhase::Error, duty.phase());
  TEST_ASSERT_EQUAL(DutyCycleError::SensorBeginFailed, duty.error());

  TEST_ASSERT_EQUAL(1, imu.beginCount);
  TEST_ASSERT_EQUAL(0, imu.sleepCount);
  TEST_ASSERT_EQUAL(0, imu.wakeCount);
}

void test_begin_enters_error_if_sensor_sleep_fails() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.sleepOk = false;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_FALSE(duty.begin());

  TEST_ASSERT_EQUAL(DutyCyclePhase::Error, duty.phase());

  TEST_ASSERT_EQUAL(1, imu.beginCount);
  TEST_ASSERT_EQUAL(1, imu.sleepCount);
  TEST_ASSERT_EQUAL(0, imu.wakeCount);
}

void test_begin_enters_error_if_sensor_wake_fails() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.wakeOk = false;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_FALSE(duty.begin());

  TEST_ASSERT_EQUAL(DutyCyclePhase::Error, duty.phase());
  TEST_ASSERT_EQUAL(DutyCycleError::SensorWakeFailed, duty.error());

  TEST_ASSERT_EQUAL(1, imu.beginCount);
  TEST_ASSERT_EQUAL(1, imu.sleepCount);
  TEST_ASSERT_EQUAL(1, imu.wakeCount);
}

void test_warmup_services_sensors_but_does_not_transition_before_warmup_time() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());

  clock.advance(cfg.warmupMs - 1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());
  TEST_ASSERT_EQUAL(1, imu.serviceCount);
}

void test_warmup_transitions_to_active_sampling_after_warmup_time() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  clock.advance(cfg.warmupMs);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());
}

void test_active_sampling_samples_ready_sensors_and_sets_telemetry_ready() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());

  clock.advance(cfg.warmupMs);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());

  const uint16_t serviceBefore = imu.serviceCount;
  const uint16_t sampleBefore = imu.sampleCount;

  clock.advance(cfg.samplePeriodMs + 1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());
  TEST_ASSERT_EQUAL(serviceBefore + 1, imu.serviceCount);
  TEST_ASSERT_EQUAL(sampleBefore + 1, imu.sampleCount);
  TEST_ASSERT_TRUE(duty.telemetryReady());
}

void test_active_sampling_does_not_sample_unready_sensor() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = false;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.samplePeriodMs);
  duty.update();

  TEST_ASSERT_EQUAL(0, imu.sampleCount);
  TEST_ASSERT_FALSE(duty.telemetryReady());
}

void test_disabled_duty_cycle_continues_sampling_past_active_window() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  cfg.enabled = false;
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.activeSampleMs + 1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());
  TEST_ASSERT_TRUE(duty.telemetryReady());

  duty.markTelemetrySent();
  TEST_ASSERT_FALSE(duty.telemetryReady());

  const uint16_t sampleBefore = imu.sampleCount;

  clock.advance(cfg.samplePeriodMs + 1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());
  TEST_ASSERT_EQUAL(sampleBefore + 1, imu.sampleCount);
  TEST_ASSERT_TRUE(duty.telemetryReady());
}

void test_mark_telemetry_sent_clears_telemetry_ready() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.samplePeriodMs);
  duty.update();

  TEST_ASSERT_TRUE(duty.telemetryReady());

  duty.markTelemetrySent();

  TEST_ASSERT_FALSE(duty.telemetryReady());
  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());
}

void test_active_sampling_enters_cooldown_after_active_sample_time() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);

  TEST_ASSERT_EQUAL(2, imu.sleepCount);
  TEST_ASSERT_FALSE(duty.telemetryReady());
}

void test_cooldown_services_trigger_sensor_before_min_sleep_time() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);

  clock.advance(cfg.minSleepMs - 1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());
  TEST_ASSERT_EQUAL(1, trigger.serviceCount);
  TEST_ASSERT_EQUAL(1, trigger.sampleCount);
}

void test_cooldown_returns_to_idle_sleeping_after_min_sleep_time() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);

  clock.advance(cfg.minSleepMs - 1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());

  clock.advance(1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());
}

void test_idle_sleeping_sets_baseline_without_waking_on_first_valid_reading() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(20.0f, 50.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);
  moveFromCooldownToIdle(duty, clock, cfg);

  // const uint16_t wakeCountBefore = imu.wakeCount;
  //
  // duty.update();
  //
  // TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());
  // TEST_ASSERT_EQUAL(wakeCountBefore, imu.wakeCount);
  // TEST_ASSERT_EQUAL(1, trigger.serviceCount);
  // TEST_ASSERT_EQUAL(1, trigger.sampleCount);
  const uint16_t serviceBefore = trigger.serviceCount;
  const uint16_t sampleBefore = trigger.sampleCount;
  const uint16_t wakeCountBefore = imu.wakeCount;

  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());
  TEST_ASSERT_EQUAL(wakeCountBefore, imu.wakeCount);
  TEST_ASSERT_EQUAL(serviceBefore + 1, trigger.serviceCount);
  TEST_ASSERT_EQUAL(sampleBefore + 1, trigger.sampleCount);
}

void test_idle_sleeping_wakes_when_temperature_threshold_crosses() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(20.0f, 50.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);
  moveFromCooldownToIdle(duty, clock, cfg);

  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());

  trigger.setReading(21.5f, 50.0f, true);

  clock.advance(cfg.minSleepMs);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());
  TEST_ASSERT_EQUAL(2, imu.wakeCount);
}

void test_idle_sleeping_wakes_when_humidity_threshold_crosses() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(20.0f, 50.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);
  moveFromCooldownToIdle(duty, clock, cfg);

  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());

  trigger.setReading(20.0f, 56.0f, true);

  clock.advance(cfg.minSleepMs);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());
  TEST_ASSERT_EQUAL(2, imu.wakeCount);
}

void test_idle_sleeping_does_not_wake_before_min_sleep_even_if_threshold_crosses() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(20.0f, 50.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);
  moveFromCooldownToIdle(duty, clock, cfg);

  duty.update();

  trigger.setReading(25.0f, 50.0f, true);

  clock.advance(cfg.minSleepMs - 1);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());
  TEST_ASSERT_EQUAL(1, imu.wakeCount);
}

void test_invalid_trigger_reading_does_not_establish_baseline_or_wake() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(20.0f, 50.0f, false);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);
  moveFromActiveToCooldown(duty, clock, cfg);
  moveFromCooldownToIdle(duty, clock, cfg);

  duty.update();

  trigger.setReading(30.0f, 80.0f, true);

  clock.advance(cfg.minSleepMs);
  duty.update();

  // Because the first invalid reading should not establish a baseline,
  // this valid reading should establish baseline instead of triggering wake.
  TEST_ASSERT_EQUAL(DutyCyclePhase::IdleSleeping, duty.phase());
  TEST_ASSERT_EQUAL(1, imu.wakeCount);
}

void test_null_sensor_entries_are_ignored() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);

  ISensor *sensors[] = {
      nullptr,
      &imu,
      nullptr,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 3, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());

  TEST_ASSERT_EQUAL(1, imu.beginCount);
  TEST_ASSERT_EQUAL(1, imu.sleepCount);
  TEST_ASSERT_EQUAL(1, imu.wakeCount);
}

void test_sample_failure_does_not_set_telemetry_ready() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;
  imu.sampleOk = false;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.samplePeriodMs + 1);
  duty.update();

  TEST_ASSERT_EQUAL(1, imu.sampleCount);
  TEST_ASSERT_FALSE(duty.telemetryReady());
}

void test_null_sensor_entries_are_skipped_during_begin_and_sampling() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {
      nullptr,
      &imu,
      nullptr,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 3, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  TEST_ASSERT_EQUAL(DutyCyclePhase::WarmingUp, duty.phase());

  TEST_ASSERT_EQUAL(1, imu.beginCount);
  TEST_ASSERT_EQUAL(1, imu.sleepCount);
  TEST_ASSERT_EQUAL(1, imu.wakeCount);

  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.samplePeriodMs + 1);
  duty.update();

  TEST_ASSERT_EQUAL(1, imu.sampleCount);
  TEST_ASSERT_TRUE(duty.telemetryReady());
}

void test_always_on_sensors_are_sampled_but_not_sleep_wake_managed() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor sht31("sht31", SensorDutyClass::AlwaysOn);
  sht31.readyValue = true;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {
      &sht31,
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 2, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  TEST_ASSERT_EQUAL(1, sht31.beginCount);
  TEST_ASSERT_EQUAL(1, imu.beginCount);

  TEST_ASSERT_EQUAL(0, sht31.sleepCount);
  TEST_ASSERT_EQUAL(0, sht31.wakeCount);

  TEST_ASSERT_EQUAL(1, imu.sleepCount);
  TEST_ASSERT_EQUAL(1, imu.wakeCount);

  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.samplePeriodMs + 1);
  duty.update();

  TEST_ASSERT_EQUAL(1, sht31.sampleCount);
  TEST_ASSERT_EQUAL(1, imu.sampleCount);

  TEST_ASSERT_TRUE(duty.telemetryReady());
}

void test_active_sampling_calls_service_before_ready_and_sample() {
  FakeClock clock;
  FakeTriggerSensor trigger;

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {
      &imu,
  };

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());

  moveFromWarmingToActive(duty, clock, cfg);

  const uint16_t serviceBefore = imu.serviceCount;
  const uint16_t sampleBefore = imu.sampleCount;

  clock.advance(cfg.samplePeriodMs + 1);
  duty.update();

  TEST_ASSERT_EQUAL(serviceBefore + 1, imu.serviceCount);
  TEST_ASSERT_EQUAL(sampleBefore + 1, imu.sampleCount);
  TEST_ASSERT_TRUE(duty.telemetryReady());
}

// -----------------------------------------------------------------------------
// Full-bundle active window + fixed cycle period
// -----------------------------------------------------------------------------

// Timed profile: the window is held open past activeSampleMs while the caller
// reports a partial bundle, and the standby is the remainder of a fixed period.
static DutyCycleConfig makeTimedConfig() {
  DutyCycleConfig cfg = makeTestConfig();

  cfg.wakeMode = DutyCycleMode::Timed;
  cfg.minSleepMs = 0;
  cfg.cyclePeriodMs = 1000;
  cfg.minStandbyMs = 50;
  cfg.activeOverrunMaxMs = 30;

  return cfg;
}

// The window used to close *after* taking a sample in the same update(), and
// transitionTo(CooldownSleeping) clears _freshSampleReady — so the sensor read
// on the closing tick was paid for and then discarded. With activeSampleMs an
// exact multiple of samplePeriodMs that was every window.
void test_closing_tick_does_not_sample_and_discard() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {&imu};

  DutyCycleConfig cfg = makeTestConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  moveFromWarmingToActive(duty, clock, cfg);

  const uint16_t samplesBefore = imu.sampleCount;

  // Land exactly on the closing boundary.
  clock.advance(cfg.activeSampleMs);
  duty.update();

  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());
  TEST_ASSERT_EQUAL(samplesBefore, imu.sampleCount);
  TEST_ASSERT_FALSE(duty.telemetryReady());
}

void test_active_window_is_held_open_while_a_bundle_is_partial() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {&imu};

  DutyCycleConfig cfg = makeTimedConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  moveFromWarmingToActive(duty, clock, cfg);

  duty.setActiveWindowHold(true);

  clock.advance(cfg.activeSampleMs);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());

  // The bundle completes; the very next tick may close the window.
  duty.setActiveWindowHold(false);
  clock.advance(cfg.samplePeriodMs);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());
  TEST_ASSERT_FALSE(duty.lastWindowOverran());
}

// A starved sample tick must not hold the window open forever — past the cap the
// window closes anyway and the caller force-encodes the runt.
void test_active_window_hold_is_bounded_by_the_overrun_cap() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {&imu};

  DutyCycleConfig cfg = makeTimedConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  moveFromWarmingToActive(duty, clock, cfg);

  // Hold asserted and never released.
  duty.setActiveWindowHold(true);

  clock.advance(cfg.activeSampleMs + cfg.activeOverrunMaxMs - 1);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::ActiveSampling, duty.phase());

  clock.advance(2);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());

  // Flagged so the app knows to flush the partial bundle rather than assume the
  // window landed on a boundary.
  TEST_ASSERT_TRUE(duty.lastWindowOverran());
}

// Fixed period: an overrun comes out of the standby, so wake-to-wake stays put.
void test_planned_sleep_is_the_remainder_of_a_fixed_cycle_period() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {&imu};

  DutyCycleConfig cfg = makeTimedConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.activeSampleMs);
  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());

  const uint32_t awakeMs = cfg.warmupMs + cfg.activeSampleMs;
  TEST_ASSERT_EQUAL_UINT32(cfg.cyclePeriodMs - awakeMs, duty.plannedSleepMs());

  // Time spent awake after the close (waiting for a TDMA slot to carry the last
  // bundle and PKT_WINDOW_END) is charged to the standby, not added to the cycle.
  clock.advance(20);
  TEST_ASSERT_EQUAL_UINT32(cfg.cyclePeriodMs - awakeMs - 20,
                           duty.timedSleepRemainingMs());
}

// A window that overruns badly must still sleep, even if that stretches the
// period — never collapse the standby to nothing.
void test_planned_sleep_is_floored_when_the_cycle_overruns() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {&imu};

  DutyCycleConfig cfg = makeTimedConfig();
  cfg.cyclePeriodMs = cfg.warmupMs + cfg.activeSampleMs;  // no slack at all
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  moveFromWarmingToActive(duty, clock, cfg);

  clock.advance(cfg.activeSampleMs);
  duty.update();

  TEST_ASSERT_EQUAL_UINT32(cfg.minStandbyMs, duty.plannedSleepMs());
}

void test_window_sample_count_is_reported_for_the_closed_window() {
  FakeClock clock;
  FakeTriggerSensor trigger;
  trigger.setReading(22.0f, 40.0f, true);

  FakeSensor imu("imu", SensorDutyClass::DutyCycled);
  imu.readyValue = true;

  ISensor *sensors[] = {&imu};

  DutyCycleConfig cfg = makeTimedConfig();
  TestBattery battery(clock);
  DutyCycleController duty(cfg, trigger, sensors, 1, clock, battery.monitor);

  TEST_ASSERT_TRUE(duty.begin());
  moveFromWarmingToActive(duty, clock, cfg);

  // transitionTo(ActiveSampling) forces an immediate first sample, then one per
  // samplePeriodMs up to (but not including) the closing tick.
  uint16_t expected = 0;
  for (uint32_t t = 0; t < cfg.activeSampleMs; t += cfg.samplePeriodMs) {
    duty.update();
    expected++;
    clock.advance(cfg.samplePeriodMs);
  }

  duty.update();
  TEST_ASSERT_EQUAL(DutyCyclePhase::CooldownSleeping, duty.phase());
  TEST_ASSERT_EQUAL_UINT16(expected, duty.lastWindowSampleCount());
}

void setUp() {}

void tearDown() {}

int main() {
  delay(2000);

  UNITY_BEGIN();

  RUN_TEST(test_begin_starts_warmup_after_begin_sleep_wake_sequence);
  RUN_TEST(test_begin_enters_error_if_sensor_begin_fails);
  RUN_TEST(test_begin_enters_error_if_sensor_sleep_fails);
  RUN_TEST(test_begin_enters_error_if_sensor_wake_fails);

  RUN_TEST(
      test_warmup_services_sensors_but_does_not_transition_before_warmup_time);
  RUN_TEST(test_warmup_transitions_to_active_sampling_after_warmup_time);

  RUN_TEST(test_active_sampling_samples_ready_sensors_and_sets_telemetry_ready);
  RUN_TEST(test_active_sampling_does_not_sample_unready_sensor);
  RUN_TEST(test_disabled_duty_cycle_continues_sampling_past_active_window);
  RUN_TEST(test_mark_telemetry_sent_clears_telemetry_ready);
  RUN_TEST(test_active_sampling_enters_cooldown_after_active_sample_time);

  RUN_TEST(test_cooldown_services_trigger_sensor_before_min_sleep_time);
  RUN_TEST(test_cooldown_returns_to_idle_sleeping_after_min_sleep_time);

  RUN_TEST(
      test_idle_sleeping_sets_baseline_without_waking_on_first_valid_reading);
  RUN_TEST(test_idle_sleeping_wakes_when_temperature_threshold_crosses);
  RUN_TEST(test_idle_sleeping_wakes_when_humidity_threshold_crosses);
  RUN_TEST(
      test_idle_sleeping_does_not_wake_before_min_sleep_even_if_threshold_crosses);
  RUN_TEST(test_invalid_trigger_reading_does_not_establish_baseline_or_wake);

  RUN_TEST(test_null_sensor_entries_are_ignored);

  RUN_TEST(test_sample_failure_does_not_set_telemetry_ready);
  RUN_TEST(test_active_sampling_calls_service_before_ready_and_sample);

  RUN_TEST(test_closing_tick_does_not_sample_and_discard);
  RUN_TEST(test_active_window_is_held_open_while_a_bundle_is_partial);
  RUN_TEST(test_active_window_hold_is_bounded_by_the_overrun_cap);
  RUN_TEST(test_planned_sleep_is_the_remainder_of_a_fixed_cycle_period);
  RUN_TEST(test_planned_sleep_is_floored_when_the_cycle_overruns);
  RUN_TEST(test_window_sample_count_is_reported_for_the_closed_window);
  RUN_TEST(test_null_sensor_entries_are_skipped_during_begin_and_sampling);
  RUN_TEST(test_always_on_sensors_are_sampled_but_not_sleep_wake_managed);
  UNITY_END();
  return 0;
}
