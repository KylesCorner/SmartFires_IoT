// Tripwire test for the config/ consolidation (see
// documentation/Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md).
//
// This does not re-verify the static_asserts already enforced at compile
// time in NetworkConfig.h (those fail the build directly if violated). It
// instead checks two things that *can* drift silently at runtime:
//
//   1. Each "thin wrapper" factory (TdmaConfig/RadioHeadTdmaDriver/
//      DutyCycleConfig/BatteryMonitor::Config/each sensor's Config) actually
//      produces a struct whose fields equal the named constants in
//      config/*.h — i.e. the wrapper really is thin, not silently
//      re-introducing its own divergent default.
//   2. The per-sensor values consolidated into SensingConfig.h equal the
//      specific tuned values each sensor shipped with before this
//      consolidation — so an accidental edit to one sensor's constant shows
//      up here instead of only being noticed in the field.

#include <unity.h>

#include "config/BaseConfig.h"
#include "config/NetworkConfig.h"
#include "config/PowerConfig.h"
#include "config/SensingConfig.h"

#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"

// NOTE: RadioHeadTdmaDriver::Config::radioHeadCfg() is intentionally NOT
// exercised here — platform/RadioHeadTdmaDriver.h pulls in <Arduino.h>,
// <RHReliableDatagram.h>, and <RH_RF95.h>, none of which are available
// under the native test platform (no RadioHead lib_deps, no shim for it
// under test/support/, unlike the lightweight test/support/Arduino.h shim
// that lets power/ and sensors/ build natively). The
// retries/timeoutMs == NetworkConfig::kLinkRetries/kLinkAckTimeoutMs claim
// was verified by direct code review instead — see RadioHeadTdmaDriver.h.

#include "sensors/Icm20948Sensor.h"
#include "sensors/Pa1010dGpsSensor.h"
#include "sensors/Sht31Sensor.h"
#include "sensors/Sps30Sensor.h"
#include "sensors/WindSensorRevC.h"

// -----------------------------------------------------------------------------
// Network domain
// -----------------------------------------------------------------------------

void test_node_tdma_profile_matches_network_config(void) {
  TdmaConfig cfg = NetworkConfig::nodeTdmaProfile();

  TEST_ASSERT_EQUAL_UINT8(0, cfg.nodeId);
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kBaseAddr, cfg.baseAddr);
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kNumSlots, cfg.numSlots);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kSlotWidthMs, cfg.slotWidthMs);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kGuardMs, cfg.guardMs);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kSyncStaleMs, cfg.syncStaleMs);
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kQueueDepth, cfg.queueDepth);
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kLinkRetries, cfg.maxRetries);
  TEST_ASSERT_EQUAL_UINT16(NetworkConfig::kLinkAckTimeoutMs, cfg.ackTimeoutMs);
  TEST_ASSERT_TRUE(cfg.enableAppReliability);
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kReliabilityWindowDepth,
                          cfg.reliabilityWindowDepth);
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kReliabilityMaxAttempts,
                          cfg.reliabilityMaxAttempts);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kReliabilityMaxAgeMs,
                           cfg.reliabilityMaxAgeMs);
  TEST_ASSERT_EQUAL(static_cast<int>(NetworkConfig::kReliabilityMode),
                    static_cast<int>(cfg.reliabilityMode));
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kExpectedAckIntervalMs,
                           cfg.expectedAckIntervalMs);
  TEST_ASSERT_EQUAL_UINT16(NetworkConfig::kRetryWaitMultiplierPermille,
                           cfg.retryWaitMultiplierPermille);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kRetryWaitMinMs, cfg.retryWaitMinMs);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kRetryWaitMaxMs, cfg.retryWaitMaxMs);
  TEST_ASSERT_EQUAL(NetworkConfig::kRequireAckSummaryBeforeFirstRetry,
                    cfg.requireAckSummaryBeforeFirstRetry);

  // enableLinkAck is derived from reliabilityMode, not an independent value.
  const bool expectStrictLinkAck =
      (NetworkConfig::kReliabilityMode == TdmaReliabilityMode::StrictLinkAck);
  TEST_ASSERT_EQUAL(expectStrictLinkAck, cfg.enableLinkAck);
}

void test_base_config_shares_geometry_with_network_config(void) {
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kNumSlots, BaseConfig::kTdmaNumSlots);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kSlotWidthMs, BaseConfig::kTdmaSlotWidthMs);
  TEST_ASSERT_EQUAL_UINT32(NetworkConfig::kGuardMs, BaseConfig::kTdmaGuardMs);

  // kTotalEntities must track kNumSlots, not be an independent literal —
  // this is the fix for the node-assignment table sizing drift.
  TEST_ASSERT_EQUAL_UINT8(NetworkConfig::kNumSlots, BaseConfig::kTotalEntities);
  TEST_ASSERT_EQUAL_UINT8(BaseConfig::kTotalEntities - 1, BaseConfig::kMaxAssignedNodes);
}

// -----------------------------------------------------------------------------
// Power domain
// -----------------------------------------------------------------------------

void test_battery_config_matches_power_config(void) {
  BatteryMonitor::Config cfg = BatteryMonitor::Config::makeBatConfig();

  TEST_ASSERT_EQUAL_FLOAT(PowerConfig::Battery::kAdcRefVolts, cfg.adcRefVolts);
  TEST_ASSERT_EQUAL_UINT16(PowerConfig::Battery::kAdcMax, cfg.adcMax);
  TEST_ASSERT_EQUAL_FLOAT(PowerConfig::Battery::kDividerRatio, cfg.dividerRatio);
  TEST_ASSERT_EQUAL_FLOAT(PowerConfig::Battery::kMinVoltage, cfg.minVoltage);
  TEST_ASSERT_EQUAL_FLOAT(PowerConfig::Battery::kMaxVoltage, cfg.maxVoltage);
  TEST_ASSERT_EQUAL_FLOAT(PowerConfig::Battery::kLowVoltage, cfg.lowVoltage);
  TEST_ASSERT_EQUAL_UINT32(PowerConfig::Battery::kMinSamplePeriodMs,
                           cfg.minSamplePeriodMs);
}

// -----------------------------------------------------------------------------
// Sensing domain — duty cycle
// -----------------------------------------------------------------------------

void test_duty_cycle_continuous_matches_sensing_config(void) {
  DutyCycleConfig cfg = DutyCycleConfig::dutyCycleCfgContinuous();

  TEST_ASSERT_EQUAL(SensingConfig::DutyCycle::kContinuousEnabled, cfg.enabled);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kContinuousSamplePeriodMs,
                           cfg.samplePeriodMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kContinuousWarmupMs,
                           cfg.warmupMs);
}

void test_duty_cycle_threshold_matches_sensing_config(void) {
  DutyCycleConfig cfg = DutyCycleConfig::dutyCycleCfg();

  TEST_ASSERT_EQUAL(SensingConfig::DutyCycle::kThresholdEnabled, cfg.enabled);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kThresholdMinSleepMs,
                           cfg.minSleepMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::DutyCycle::kThresholdSamplePeriodMs,
                           cfg.samplePeriodMs);
  TEST_ASSERT_EQUAL_FLOAT(SensingConfig::DutyCycle::kThresholdTempDeltaThresholdC,
                          cfg.tempDeltaThresholdC);
}

// -----------------------------------------------------------------------------
// Sensing domain — per-sensor (each block is that sensor's own distinct,
// independently-tuned value; deliberately not generalized into one shared
// constant, per the consolidation's explicit instruction).
// -----------------------------------------------------------------------------

void test_sht31_config_matches_sensing_config(void) {
  Sht31Sensor::Config cfg = Sht31Sensor::Config::makeSht31Cfg();

  TEST_ASSERT_EQUAL_UINT8(SensingConfig::Sht31::kAddress, cfg.address);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Sht31::kMinSamplePeriodMs,
                           cfg.minSamplePeriodMs);
  TEST_ASSERT_EQUAL(static_cast<int>(SensingConfig::Sht31::kDutyClass),
                    static_cast<int>(cfg.dutyClass));
}

void test_wind_config_matches_sensing_config(void) {
  WindSensorRevC::Config cfg = WindSensorRevC::Config::makeRevCCfg(1, 2);

  TEST_ASSERT_EQUAL_FLOAT(SensingConfig::Wind::kDividerRatio, cfg.rvDividerRatio);
  TEST_ASSERT_EQUAL_FLOAT(SensingConfig::Wind::kDividerRatio, cfg.tmpDividerRatio);
  TEST_ASSERT_EQUAL_FLOAT(SensingConfig::Wind::kZeroWindAdjustmentVolts,
                          cfg.zeroWindAdjustmentVolts);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Wind::kMinSamplePeriodMs,
                           cfg.minSamplePeriodMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Wind::kWakeDelayMs, cfg.wakeDelayMs);
  TEST_ASSERT_EQUAL(static_cast<int>(SensingConfig::Wind::kDutyClass),
                    static_cast<int>(cfg.dutyClass));
}

void test_sps30_config_matches_sensing_config(void) {
  Sps30Sensor::Config cfg = Sps30Sensor::Config::makeSps30Cfg();

  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Sps30::kMinSamplePeriodMs,
                           cfg.minSamplePeriodMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Sps30::kWakeDelayMs, cfg.wakeDelayMs);
  TEST_ASSERT_EQUAL(static_cast<int>(SensingConfig::Sps30::kDutyClass),
                    static_cast<int>(cfg.dutyClass));
}

void test_imu_config_matches_sensing_config(void) {
  Icm20948Sensor::Config cfg = Icm20948Sensor::Config::makeImuCfg();

  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Imu::kMinSamplePeriodMs,
                           cfg.minSamplePeriodMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Imu::kWakeDelayMs, cfg.wakeDelayMs);
  TEST_ASSERT_EQUAL(static_cast<int>(SensingConfig::Imu::kDutyClass),
                    static_cast<int>(cfg.dutyClass));
  TEST_ASSERT_EQUAL_UINT8(SensingConfig::Imu::kAddress, cfg.address);
}

void test_gps_periodic_backup_config_matches_sensing_config(void) {
  Pa1010dGpsSensor::Config cfg = Pa1010dGpsSensor::Config::makePeriodicBackupCfg();

  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Gps::kPeriodicRunTimeMs,
                           cfg.periodic.runTimeMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Gps::kPeriodicSleepTimeMs,
                           cfg.periodic.sleepTimeMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Gps::kPeriodicSecondRunTimeMs,
                           cfg.periodic.secondRunTimeMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Gps::kPeriodicSecondSleepTimeMs,
                           cfg.periodic.secondSleepTimeMs);
  TEST_ASSERT_EQUAL_UINT32(SensingConfig::Gps::kPeriodicMinSamplePeriodMs,
                           cfg.minSamplePeriodMs);
  TEST_ASSERT_EQUAL_UINT8(SensingConfig::Gps::kAddress, cfg.address);
  TEST_ASSERT_EQUAL(static_cast<int>(GpsPowerMode::PeriodicBackup),
                    static_cast<int>(cfg.powerMode));
}

// -----------------------------------------------------------------------------
// runner
// -----------------------------------------------------------------------------

void runConfigTests() {
  UNITY_BEGIN();

  RUN_TEST(test_node_tdma_profile_matches_network_config);
  RUN_TEST(test_base_config_shares_geometry_with_network_config);

  RUN_TEST(test_battery_config_matches_power_config);

  RUN_TEST(test_duty_cycle_continuous_matches_sensing_config);
  RUN_TEST(test_duty_cycle_threshold_matches_sensing_config);

  RUN_TEST(test_sht31_config_matches_sensing_config);
  RUN_TEST(test_wind_config_matches_sensing_config);
  RUN_TEST(test_sps30_config_matches_sensing_config);
  RUN_TEST(test_imu_config_matches_sensing_config);
  RUN_TEST(test_gps_periodic_backup_config_matches_sensing_config);

  UNITY_END();
}

#ifdef ARDUINO

void setup() {
  delay(2000);
  runConfigTests();
}

void loop() {}

#else

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  runConfigTests();

  return 0;
}

#endif
