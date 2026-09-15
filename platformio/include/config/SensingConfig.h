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
// table and documentation/Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md
// for the consolidation rationale.
//
// Data only — no logic, no driver includes.

#include "interfaces/ISensor.h"
#include "power/DutyCycleController.h"
#include "telemetry/BinaryPacket.h"

#include <stdint.h>

// ---------------------------------------------------------------------------
// Build-flag resolution
// ---------------------------------------------------------------------------
//
// SMARTFIRES_DUTY_CYCLE_MODE selects one complete controller mode:
//
//   0 = Continuous
//   1 = SensorTriggered
//   2 = Timed
//   3 = Hybrid
//
// The selected profile is resolved below into the kActive* constants consumed
// by main.cpp when constructing DutyCycleConfig.
//
// ---------------------------------------------------------------------------
// Duty cycle mode selection
// ---------------------------------------------------------------------------
//
// PlatformIO build flag:
//
//   -DSMARTFIRES_DUTY_CYCLE_MODE=0  Continuous
//   -DSMARTFIRES_DUTY_CYCLE_MODE=1  Sensor-triggered
//   -DSMARTFIRES_DUTY_CYCLE_MODE=2  Timed
//   -DSMARTFIRES_DUTY_CYCLE_MODE=3  Hybrid
//
#define SMARTFIRES_DUTY_MODE_CONTINUOUS       0
#define SMARTFIRES_DUTY_MODE_SENSOR_TRIGGERED 1
#define SMARTFIRES_DUTY_MODE_TIMED            2
#define SMARTFIRES_DUTY_MODE_HYBRID           3

#ifndef SMARTFIRES_DUTY_CYCLE_MODE
#define SMARTFIRES_DUTY_CYCLE_MODE \
  SMARTFIRES_DUTY_MODE_SENSOR_TRIGGERED
#endif

namespace SensingConfig {
// ---------------------------------------------------------------------------
// Duty cycle (DutyCycleController)
// ---------------------------------------------------------------------------
namespace DutyCycle {

// Shortest remaining sleep worth entering MCU standby for (RTC MODE0 gives
// ~1 ms alarm resolution; below this the enter/exit overhead isn't worth it).
constexpr uint32_t kMinMcuStandbyMs = 250;

// How long the node stays awake at the end of an active window waiting for the
// TX queue to drain before entering standby anyway. The window's final bundle
// and its PKT_WINDOW_END marker are enqueued at window close and need this
// node's TDMA slot, which comes around once per frame (NUM_SLOTS × slotWidthMs
// = 3.6 s at the default geometry), so this covers roughly one frame plus
// main-loop slack. Exceeding it means the base is unreachable, in which case
// sleeping is better than burning the window's power budget waiting — the base
// then falls back to BaseConfig::kAckSummaryNodeSilenceMs to notice the node is
// gone without ever seeing its WINDOW_END.
constexpr uint32_t kMaxTxDrainBeforeStandbyMs = 5000;

// Samples in one full PKT_BUNDLE: the FullState reference plus its deltas.
// PacketHandler encodes a bundle every time this many samples have accumulated.
constexpr uint32_t kSamplesPerBundle =
    static_cast<uint32_t>(BinaryPacket::kBundleMaxDeltas) + 1u;   // 15

// ---------------------------------------------------------------------------
// Continuous profile
// ---------------------------------------------------------------------------
//
// Sensors warm up once, enter ActiveSampling, and never intentionally sleep.
//
// Sleep duration, active-window duration, timed wakeup, and trigger thresholds
// are ignored in this mode.
//
constexpr DutyCycleMode kContinuousMode =
    DutyCycleMode::Continuous;

constexpr uint32_t kContinuousMinSleepMs = 0;
constexpr uint32_t kContinuousMaxWakeMs = 0;
constexpr uint32_t kContinuousActiveSampleMs = 0;
constexpr uint32_t kContinuousSamplePeriodMs = 750;
constexpr uint32_t kContinuousWarmupMs = 10000;
constexpr uint32_t kContinuousCyclePeriodMs = 0;
constexpr uint32_t kContinuousMinStandbyMs = 0;
constexpr uint32_t kContinuousActiveOverrunMaxMs = 0;

constexpr float kContinuousTempDeltaThresholdC = 0.0f;
constexpr float kContinuousHumidityDeltaThresholdPct = 0.0f;

constexpr bool kContinuousFailOnSampleError = false;

// ---------------------------------------------------------------------------
// Sensor-triggered profile
// ---------------------------------------------------------------------------
//
// Sensors sample for kSensorTriggeredActiveSampleMs, then sleep.
//
// They wake only when the trigger sensor crosses a configured threshold.
// There is no scheduled timer wakeup.
//
constexpr DutyCycleMode kSensorTriggeredMode =
    DutyCycleMode::SensorTriggered;

constexpr uint32_t kSensorTriggeredMinSleepMs = 3000;
constexpr uint32_t kSensorTriggeredMaxWakeMs = 1000;
constexpr uint32_t kSensorTriggeredActiveSampleMs = 30000;
constexpr uint32_t kSensorTriggeredSamplePeriodMs = 750;
constexpr uint32_t kSensorTriggeredWarmupMs = 10000;

// Ignored in SensorTriggered mode.
constexpr uint32_t kSensorTriggeredCyclePeriodMs = 0;
constexpr uint32_t kSensorTriggeredMinStandbyMs = 0;
constexpr uint32_t kSensorTriggeredActiveOverrunMaxMs = 0;

constexpr float kSensorTriggeredTempDeltaThresholdC = 1.0f;
constexpr float kSensorTriggeredHumidityDeltaThresholdPct = 5.0f;

constexpr bool kSensorTriggeredFailOnSampleError = false;

// ---------------------------------------------------------------------------
// Timed profile
// ---------------------------------------------------------------------------
//
// Sensors sample for kTimedActiveSampleMs (extended to the next whole bundle),
// then standby for whatever is left of kTimedCyclePeriodMs.
//
// Trigger-sensor thresholds do not cause a wakeup in this mode.
//
constexpr DutyCycleMode kTimedMode =
    DutyCycleMode::Timed;

constexpr uint32_t kTimedMinSleepMs = 0;
constexpr uint32_t kTimedMaxWakeMs = 1000;
constexpr uint32_t kTimedSamplePeriodMs = 1000;
constexpr uint32_t kTimedWarmupMs = 10000;

// Whole bundles the active window is sized to produce. The window always runs
// to a bundle boundary — DutyCycleController holds it open past
// kTimedActiveSampleMs until PacketHandler's accumulator completes its bundle —
// so expressing the window as a whole number of bundles means the hold normally
// has nothing to wait for and the overrun is zero.
//
// Deriving this from kSamplesPerBundle rather than writing 30000 by hand is
// deliberate: a bare constant silently desynchronises the moment
// BinaryPacket::kBundleMaxDeltas or kTimedSamplePeriodMs changes, and a
// desynchronised window means every window ends on a runt bundle again.
constexpr uint32_t kTimedBundlesPerWindow = 2;
constexpr uint32_t kTimedActiveSampleMs =
    kTimedBundlesPerWindow * kSamplesPerBundle * kTimedSamplePeriodMs;  // 30000

// Ceiling on how far past kTimedActiveSampleMs the full-bundle hold may run.
// Only reachable when the sample tick is being starved (a wedged sensor, a
// blocking I2C/UART read overrunning the period); at that point PacketHandler
// force-encodes the partial bundle rather than lose the samples. One bundle's
// worth of samples is the natural bound — needing more than that means the tick
// is not merely jittering.
constexpr uint32_t kTimedActiveOverrunMaxMs =
    kSamplesPerBundle * kTimedSamplePeriodMs;                          // 15000

// Fixed wake-to-wake period. The standby is the remainder after warmup, the
// active window (including any overrun) and the post-close TX drain have taken
// their share, floored at kTimedMinStandbyMs. Holding the *period* fixed rather
// than the sleep keeps the cycle length constant when the window overruns, which
// is what makes the base's return-time prediction (WindowMarkerPayload's
// planned_sleep_ms) meaningful and keeps cycles comparable across a session.
//
// 10 s warmup + 30 s window + 35 s standby. The standby matches what shipped
// before the full-bundle hold, so the power profile in POWER_MEASURMENTS.md
// still applies; the window grew by 5 s (25 -> 30) to reach the second bundle
// boundary.
constexpr uint32_t kTimedCyclePeriodMs = 75000;

// Floor on the derived standby, so an active window that overruns badly cannot
// collapse the sleep to nothing (or below kMinMcuStandbyMs, where standby stops
// being worth entering). Stretches the period rather than skipping the sleep.
constexpr uint32_t kTimedMinStandbyMs = 5000;

// The active window must be a whole number of bundles, or the full-bundle hold
// runs on every single window instead of never — which is the difference
// between a zero overrun and a systematic one.
static_assert(kTimedActiveSampleMs %
                      (kSamplesPerBundle * kTimedSamplePeriodMs) == 0,
              "Timed active window must be a whole number of bundles");

// The nominal cycle must leave room for a real standby, otherwise every cycle
// lands on the kTimedMinStandbyMs floor and the period silently stops being
// fixed. Budgets the worst-case overrun too, so a held-open window still sleeps.
static_assert(kTimedCyclePeriodMs >=
                  kTimedWarmupMs + kTimedActiveSampleMs +
                      kTimedActiveOverrunMaxMs + kTimedMinStandbyMs,
              "Timed cycle period too short for warmup + window + overrun + standby");

static_assert(kTimedMinStandbyMs >= kMinMcuStandbyMs,
              "Timed standby floor must be long enough to be worth entering");

// Ignored in Timed mode.
constexpr float kTimedTempDeltaThresholdC = 0.0f;
constexpr float kTimedHumidityDeltaThresholdPct = 0.0f;

constexpr bool kTimedFailOnSampleError = false;

// ---------------------------------------------------------------------------
// Hybrid profile
// ---------------------------------------------------------------------------
//
// Sensors sample for kHybridActiveSampleMs, then sleep.
//
// They wake when either:
//
//   - The trigger sensor crosses a threshold after kHybridMinSleepMs, or
//   - the remainder of kHybridCyclePeriodMs expires.
//
constexpr DutyCycleMode kHybridMode =
    DutyCycleMode::Hybrid;

constexpr uint32_t kHybridMinSleepMs = 3000;
constexpr uint32_t kHybridMaxWakeMs = 1000;
constexpr uint32_t kHybridActiveSampleMs = 30000;
constexpr uint32_t kHybridSamplePeriodMs = 750;
constexpr uint32_t kHybridWarmupMs = 10000;

// Fixed wake-to-wake period; the scheduled wakeup fires on whatever is left of
// it after warmup and the active window. A trigger crossing can still cut the
// sleep short at any point after kHybridMinSleepMs.
constexpr uint32_t kHybridCyclePeriodMs =
    5UL * 60UL * 1000UL + kHybridWarmupMs + kHybridActiveSampleMs;
constexpr uint32_t kHybridMinStandbyMs = 5000;

// Hybrid's sample period does not divide a bundle evenly, so the full-bundle
// hold is left off here (0 = no hold) — Timed is the duty mode the window
// markers and the bundle-boundary window are designed around.
constexpr uint32_t kHybridActiveOverrunMaxMs = 0;

constexpr float kHybridTempDeltaThresholdC = 1.0f;
constexpr float kHybridHumidityDeltaThresholdPct = 5.0f;

constexpr bool kHybridFailOnSampleError = false;

// ---------------------------------------------------------------------------
// Active profile selection
// ---------------------------------------------------------------------------

#if SMARTFIRES_DUTY_CYCLE_MODE == SMARTFIRES_DUTY_MODE_CONTINUOUS

constexpr DutyCycleMode kActiveMode =
    kContinuousMode;

constexpr uint32_t kActiveMinSleepMs =
    kContinuousMinSleepMs;
constexpr uint32_t kActiveMaxWakeMs =
    kContinuousMaxWakeMs;
constexpr uint32_t kActiveActiveSampleMs =
    kContinuousActiveSampleMs;
constexpr uint32_t kActiveSamplePeriodMs =
    kContinuousSamplePeriodMs;
constexpr uint32_t kActiveWarmupMs =
    kContinuousWarmupMs;
constexpr uint32_t kActiveCyclePeriodMs =
    kContinuousCyclePeriodMs;
constexpr uint32_t kActiveMinStandbyMs =
    kContinuousMinStandbyMs;
constexpr uint32_t kActiveActiveOverrunMaxMs =
    kContinuousActiveOverrunMaxMs;

constexpr float kActiveTempDeltaThresholdC =
    kContinuousTempDeltaThresholdC;
constexpr float kActiveHumidityDeltaThresholdPct =
    kContinuousHumidityDeltaThresholdPct;

constexpr bool kActiveFailOnSampleError =
    kContinuousFailOnSampleError;

#elif SMARTFIRES_DUTY_CYCLE_MODE == \
    SMARTFIRES_DUTY_MODE_SENSOR_TRIGGERED

constexpr DutyCycleMode kActiveMode =
    kSensorTriggeredMode;

constexpr uint32_t kActiveMinSleepMs =
    kSensorTriggeredMinSleepMs;
constexpr uint32_t kActiveMaxWakeMs =
    kSensorTriggeredMaxWakeMs;
constexpr uint32_t kActiveActiveSampleMs =
    kSensorTriggeredActiveSampleMs;
constexpr uint32_t kActiveSamplePeriodMs =
    kSensorTriggeredSamplePeriodMs;
constexpr uint32_t kActiveWarmupMs =
    kSensorTriggeredWarmupMs;
constexpr uint32_t kActiveCyclePeriodMs =
    kSensorTriggeredCyclePeriodMs;
constexpr uint32_t kActiveMinStandbyMs =
    kSensorTriggeredMinStandbyMs;
constexpr uint32_t kActiveActiveOverrunMaxMs =
    kSensorTriggeredActiveOverrunMaxMs;

constexpr float kActiveTempDeltaThresholdC =
    kSensorTriggeredTempDeltaThresholdC;
constexpr float kActiveHumidityDeltaThresholdPct =
    kSensorTriggeredHumidityDeltaThresholdPct;

constexpr bool kActiveFailOnSampleError =
    kSensorTriggeredFailOnSampleError;

#elif SMARTFIRES_DUTY_CYCLE_MODE == SMARTFIRES_DUTY_MODE_TIMED

constexpr DutyCycleMode kActiveMode =
    kTimedMode;

constexpr uint32_t kActiveMinSleepMs =
    kTimedMinSleepMs;
constexpr uint32_t kActiveMaxWakeMs =
    kTimedMaxWakeMs;
constexpr uint32_t kActiveActiveSampleMs =
    kTimedActiveSampleMs;
constexpr uint32_t kActiveSamplePeriodMs =
    kTimedSamplePeriodMs;
constexpr uint32_t kActiveWarmupMs =
    kTimedWarmupMs;
constexpr uint32_t kActiveCyclePeriodMs =
    kTimedCyclePeriodMs;
constexpr uint32_t kActiveMinStandbyMs =
    kTimedMinStandbyMs;
constexpr uint32_t kActiveActiveOverrunMaxMs =
    kTimedActiveOverrunMaxMs;

constexpr float kActiveTempDeltaThresholdC =
    kTimedTempDeltaThresholdC;
constexpr float kActiveHumidityDeltaThresholdPct =
    kTimedHumidityDeltaThresholdPct;

constexpr bool kActiveFailOnSampleError =
    kTimedFailOnSampleError;

#elif SMARTFIRES_DUTY_CYCLE_MODE == SMARTFIRES_DUTY_MODE_HYBRID

constexpr DutyCycleMode kActiveMode =
    kHybridMode;

constexpr uint32_t kActiveMinSleepMs =
    kHybridMinSleepMs;
constexpr uint32_t kActiveMaxWakeMs =
    kHybridMaxWakeMs;
constexpr uint32_t kActiveActiveSampleMs =
    kHybridActiveSampleMs;
constexpr uint32_t kActiveSamplePeriodMs =
    kHybridSamplePeriodMs;
constexpr uint32_t kActiveWarmupMs =
    kHybridWarmupMs;
constexpr uint32_t kActiveCyclePeriodMs =
    kHybridCyclePeriodMs;
constexpr uint32_t kActiveMinStandbyMs =
    kHybridMinStandbyMs;
constexpr uint32_t kActiveActiveOverrunMaxMs =
    kHybridActiveOverrunMaxMs;

constexpr float kActiveTempDeltaThresholdC =
    kHybridTempDeltaThresholdC;
constexpr float kActiveHumidityDeltaThresholdPct =
    kHybridHumidityDeltaThresholdPct;

constexpr bool kActiveFailOnSampleError =
    kHybridFailOnSampleError;

#else
#error "Invalid SMARTFIRES_DUTY_CYCLE_MODE value"
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
