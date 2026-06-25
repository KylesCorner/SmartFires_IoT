// ---
// description: BatteryMonitor's ADC/voltage threshold and sampling-cadence constants.
// role: config
// docs: [tunable-parameters]
// ---
#pragma once

// Power domain — single source of truth for BatteryMonitor's ADC/voltage
// thresholds and sampling cadence. Consolidated from
// BatteryMonitor::Config::makeBatConfig()'s positional-default factory.
//
// Data only — no logic, no Arduino includes.

namespace PowerConfig {

namespace Battery {
constexpr float kAdcRefVolts = 3.3f;
constexpr unsigned short kAdcMax = 1023;

// For voltage divider: battery -> R1 -> ADC pin -> R2 -> GND
// dividerRatio = (R1 + R2) / R2
constexpr float kDividerRatio = 2.0f;

constexpr float kMinVoltage = 3.2f;
constexpr float kMaxVoltage = 4.2f;
constexpr float kLowVoltage = 3.5f;

constexpr unsigned long kMinSamplePeriodMs = 1000;
}  // namespace Battery

}  // namespace PowerConfig
