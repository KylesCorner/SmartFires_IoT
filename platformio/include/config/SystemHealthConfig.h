// ---
// description: RAM-monitor sampling, logging, warning, and reset thresholds.
// role: config
// ---
#pragma once

#include <stdint.h>

namespace SystemHealthConfig {

namespace Ram {

// How often to actually sample the pointers.
constexpr uint32_t kSamplePeriodMs = 1000;

// Normal periodic diagnostic output.
constexpr uint32_t kLogPeriodMs = 5000;

// Initial diagnostic thresholds.
// These should be tuned after observing real hardware.
constexpr uint32_t kWarnFreeBytes = 6U * 1024U;
constexpr uint32_t kCriticalFreeBytes = 3U * 1024U;

// Log immediately when a new low-water mark drops by at least this much.
constexpr uint32_t kNewLowLogStepBytes = 256;

} // namespace Ram

namespace Watchdog {

// Armed at the very top of setup(), before any I2C/serial/radio init —
// covers the boot sequence (fixed delay, serial wait, I2C scan, sensor
// begin()/DMP init), which is longer than any single steady-state loop()
// iteration. This is the practical SAMD21 hardware ceiling; not bench-tuned.
constexpr uint32_t kBootPhaseTimeoutMs = 16000;

// Re-armed immediately before entering loop(). More than double a full TDMA
// frame (NUM_SLOTS * slotWidthMs) and more than 10x a single bundle TX, so no
// normal loop() iteration comes close to it. Starting value, not bench-tuned
// against real hardware timing yet.
constexpr uint32_t kSteadyStateTimeoutMs = 8000;

} // namespace Watchdog

} // namespace SystemHealthConfig
