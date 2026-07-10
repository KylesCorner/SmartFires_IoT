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

} // namespace SystemHealthConfig