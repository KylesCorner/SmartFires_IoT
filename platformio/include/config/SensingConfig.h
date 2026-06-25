// ---
// description: Duty-cycle cadence profiles plus each sensor's own independently-tuned sample/wake timing and calibration constants.
// role: config
// docs: [duty-cycling, tunable-parameters]
// ---
#pragma once

// Sensing domain — duty-cycle cadence profiles and each sensor's own
// independently-tuned sample/wake timing and calibration constants.
//
// IMPORTANT: every constant under a per-sensor namespace below was tuned for
// that specific piece of hardware (warm-up time, I2C/UART/ADC
// characteristics, power-rail settling, calibration offsets, etc.). These
// are deliberately NOT generalized into one shared "system sample rate" —
// each sensor keeps its own distinct values, just given one named, visible
// home instead of being buried inside that sensor's own factory-method
// default arguments (where main.cpp previously had to re-override several
// of them by hand to get the values that actually ship).
//
// Every constant here is verified to equal exactly what was previously
// either a bare factory default or a main.cpp override — this file changes
// where the value lives, not what the value is. See
// documentation/Current_Architecture/TUNABLE_PARAMETERS.md for the full
// table and documentation/Pending_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md
// for the consolidation rationale.
//
// Data only — no logic, no driver includes.

#include "interfaces/ISensor.h"

#include <stdint.h>

// ---------------------------------------------------------------------------
// Build-flag resolution
// ---------------------------------------------------------------------------
// SMARTFIRES_DUTY_CYCLE_CONTINUOUS picks which of the two DutyCycle profiles
// below main.cpp wires into DutyCycleConfig — set per-environment in
// platformio.ini (feather_m0_lora_node_debug vs feather_m0_lora_node).
//   1 = kContinuous*  — back-to-back sampling, no sleep/wake (debug env,
//       for fast iteration without waiting on duty-cycle timing)
//   0 = kThreshold*    — real sleep/wake duty cycling gated on a
//       temp/humidity threshold crossing (real node env)
// Defaults to continuous so any env that doesn't set the flag keeps the
// behavior every build has shipped with historically.
#ifndef SMARTFIRES_DUTY_CYCLE_CONTINUOUS
#define SMARTFIRES_DUTY_CYCLE_CONTINUOUS 1
#endif

namespace SensingConfig {

// ---------------------------------------------------------------------------
// Duty cycle (DutyCycleController)
// ---------------------------------------------------------------------------
// Two named profiles, matching DutyCycleController.h's two factories.
namespace DutyCycle {

// kContinuous: the debug-build profile (SMARTFIRES_DUTY_CYCLE_CONTINUOUS=1)
// — duty cycling disabled, sensors serviced back-to-back at samplePeriodMs.
constexpr bool kContinuousEnabled = false;  // false = no sleep/wake cycling; sensors run back-to-back at samplePeriodMs
constexpr uint32_t kContinuousMinSleepMs = 0;
constexpr uint32_t kContinuousMaxWakeMs = 0;
constexpr uint32_t kContinuousActiveSampleMs = 0;
constexpr uint32_t kContinuousSamplePeriodMs = 750;
constexpr uint32_t kContinuousWarmupMs = 10000;
constexpr float kContinuousTempDeltaThresholdC = 0.0f;
constexpr float kContinuousHumidityDeltaThresholdPct = 0.0f;
constexpr bool kContinuousFailOnSampleError = false;

// kThresholdTriggered: the real-node-build profile
// (SMARTFIRES_DUTY_CYCLE_CONTINUOUS=0) — trigger-based wake/sleep duty
// cycling. samplePeriodMs here carries forward a pre-existing "//TEMP"
// marker from before this consolidation; flagged as a real value, not
// silently dropped.
constexpr bool kThresholdEnabled = true;
constexpr uint32_t kThresholdMinSleepMs = 3000;
constexpr uint32_t kThresholdMaxWakeMs = 1000;
constexpr uint32_t kThresholdActiveSampleMs = 30000;
constexpr uint32_t kThresholdSamplePeriodMs = 750;  // TODO(carried over): tune this; was marked //TEMP
constexpr uint32_t kThresholdWarmupMs = 10000;
constexpr float kThresholdTempDeltaThresholdC = 1.0f;
constexpr float kThresholdHumidityDeltaThresholdPct = 5.0f;
constexpr bool kThresholdFailOnSampleError = false;

// kActive*: the profile main.cpp actually wires into DutyCycleConfig::make(),
// selected at compile time by SMARTFIRES_DUTY_CYCLE_CONTINUOUS.
#if SMARTFIRES_DUTY_CYCLE_CONTINUOUS
constexpr bool kActiveEnabled = kContinuousEnabled;
constexpr uint32_t kActiveMinSleepMs = kContinuousMinSleepMs;
constexpr uint32_t kActiveMaxWakeMs = kContinuousMaxWakeMs;
constexpr uint32_t kActiveActiveSampleMs = kContinuousActiveSampleMs;
constexpr uint32_t kActiveSamplePeriodMs = kContinuousSamplePeriodMs;
constexpr uint32_t kActiveWarmupMs = kContinuousWarmupMs;
constexpr float kActiveTempDeltaThresholdC = kContinuousTempDeltaThresholdC;
constexpr float kActiveHumidityDeltaThresholdPct = kContinuousHumidityDeltaThresholdPct;
constexpr bool kActiveFailOnSampleError = kContinuousFailOnSampleError;
#else
constexpr bool kActiveEnabled = kThresholdEnabled;
constexpr uint32_t kActiveMinSleepMs = kThresholdMinSleepMs;
constexpr uint32_t kActiveMaxWakeMs = kThresholdMaxWakeMs;
constexpr uint32_t kActiveActiveSampleMs = kThresholdActiveSampleMs;
constexpr uint32_t kActiveSamplePeriodMs = kThresholdSamplePeriodMs;
constexpr uint32_t kActiveWarmupMs = kThresholdWarmupMs;
constexpr float kActiveTempDeltaThresholdC = kThresholdTempDeltaThresholdC;
constexpr float kActiveHumidityDeltaThresholdPct = kThresholdHumidityDeltaThresholdPct;
constexpr bool kActiveFailOnSampleError = kThresholdFailOnSampleError;
#endif

}  // namespace DutyCycle

// ---------------------------------------------------------------------------
// SHT31 (temperature/humidity)
// ---------------------------------------------------------------------------
namespace Sht31 {
constexpr uint32_t kMinSamplePeriodMs = 100;
constexpr SensorDutyClass kDutyClass = SensorDutyClass::AlwaysOn;
}  // namespace Sht31

// ---------------------------------------------------------------------------
// Wind (Modern Device Rev C hot-wire anemometer)
// ---------------------------------------------------------------------------
namespace Wind {
// Both the RV and TMP divider channels on this board use the same divider
// network, hence one shared constant here (not a cross-sensor
// generalization — it is the same physical sensor's two channels).
constexpr uint32_t kMinSamplePeriodMs = 10;
constexpr uint32_t kWakeDelayMs = 10000;  // hot-wire/TPS settling
constexpr SensorDutyClass kDutyClass = SensorDutyClass::WarmupHeavy;
}  // namespace Wind

// ---------------------------------------------------------------------------
// SPS30 (particulate matter)
// ---------------------------------------------------------------------------
namespace Sps30 {
constexpr uint32_t kMinSamplePeriodMs = 1000;
constexpr uint32_t kWakeDelayMs = 8000;
constexpr SensorDutyClass kDutyClass = SensorDutyClass::WarmupHeavy;
}  // namespace Sps30

// ---------------------------------------------------------------------------
// ICM-20948 (IMU / DMP heading)
// ---------------------------------------------------------------------------
namespace Imu {
constexpr uint32_t kMinSamplePeriodMs = 10;
constexpr uint32_t kWakeDelayMs = 0;
constexpr SensorDutyClass kDutyClass = SensorDutyClass::DutyCycled;
}  // namespace Imu

// ---------------------------------------------------------------------------
// PA1010D GPS
// ---------------------------------------------------------------------------
// Three independently-tuned timing groups, one per power-mode family the
// sensor header exposes. They happen to share a couple of literal values
// today, but each is named separately so a future change to one family
// doesn't silently affect another.
namespace Gps {

// makeGpsCfg() — full-power continuous mode.
constexpr uint32_t kContinuousMinSamplePeriodMs = 100;
constexpr uint32_t kContinuousWakeDelayMs = 0;

// makePeriodicStandbyCfg() / makePeriodicBackupCfg() — periodic run/sleep
// duty cycling (currently identical between the two power sub-modes).
constexpr uint32_t kPeriodicRunTimeMs = 24000;
constexpr uint32_t kPeriodicSleepTimeMs = 90000;
constexpr uint32_t kPeriodicSecondRunTimeMs = 24000;
constexpr uint32_t kPeriodicSecondSleepTimeMs = 90000;
constexpr uint32_t kPeriodicMinSamplePeriodMs = 1000;

// makeAlwaysLocateStandbyCfg() / makeAlwaysLocateBackupCfg() — AlwaysLocate
// hardware-assisted standby (currently identical between the two).
constexpr uint32_t kAlwaysLocateMinSamplePeriodMs = 1000;
}  // namespace Gps

}  // namespace SensingConfig
