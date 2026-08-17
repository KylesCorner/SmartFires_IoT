#pragma once

#include <stdint.h>

// Pure tick math for the SAMD21 RTC in MODE0 (COUNT32) clocked at
// 1024 Hz — RTCZero's GCLK gen 2 setup (XOSC32K / 32) with the MODE0
// prescaler at DIV1. One tick ≈ 0.977 ms. No hardware dependencies so
// the math is testable on the native env.
namespace Samd21RtcTicks {

constexpr uint32_t kTicksPerSecond = 1024UL;

// Ceiling conversion so the alarm never fires before the requested
// duration. The 64-bit intermediate avoids multiply overflow; the
// returned tick count itself stays within u32 for ms <= 4,194,303,999
// (~48.5 days) — far beyond any duty-cycle sleep.
constexpr uint32_t msToTicks(uint32_t ms) {
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(ms) * kTicksPerSecond + 999ULL) / 1000ULL);
}

// Floor conversion. Paired with msToTicks()'s ceiling this round-trips
// exactly: ticksToMs(msToTicks(ms)) == ms wherever the tick count is
// representable (ms <= ~48.5 days).
constexpr uint32_t ticksToMs(uint32_t ticks) {
  return static_cast<uint32_t>(
      static_cast<uint64_t>(ticks) * 1000ULL / kTicksPerSecond);
}

// Elapsed ticks from start to end on the free-running 32-bit counter.
// Unsigned subtraction is already wraparound-safe; this names that fact.
constexpr uint32_t tickDelta(uint32_t startTicks, uint32_t endTicks) {
  return endTicks - startTicks;
}

}  // namespace Samd21RtcTicks
