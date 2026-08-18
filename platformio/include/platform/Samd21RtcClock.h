// ---
// description: IClock backed directly by the SAMD21 RTC's free-running MODE0 counter, so a node's millis() keeps advancing through MCU standby with no post-wake compensation step.
// role: implementation
// ---
#pragma once

#include "interfaces/IClock.h"
#include "platform/Samd21Rtc.h"

#include <stdint.h>

// The node's live timebase. Where ArduinoClock wraps the Arduino core's own
// tick source (which stops dead in standby, and is backed by a different and
// less stable clock tree than the RTC), this reads the crystal-backed counter
// that never stops. Awake time and asleep time therefore come from one domain,
// and there is no sleep offset to patch in on wake.
//
// Wraparound: COUNT wraps every 2^32 ticks, so millis() counts up to
// 4,194,303,999 and then restarts at 0 — about 48.5 days, against the ~49.7
// days of a u32 ::millis(). The difference is not just the shorter period:
// ::millis() wraps at exactly 2^32 ms, so the unsigned `now - then` subtraction
// every consumer uses stays correct across its wrap, whereas this clock's
// shorter ms period makes one such subtraction wrong (by ~28 h) if it happens
// to straddle the wrap. Accepted for now — a node reaching 48.5 days of
// unbroken uptime is far outside current field behavior, and fixing it means
// giving this class mutable accumulator state that millis() must be polled
// often enough to keep current.
class Samd21RtcClock final : public IClock {
public:
  explicit Samd21RtcClock(const Samd21Rtc &rtc);

  // Costs a synchronized RTC read (~150 us) — cheap enough for the hot path
  // only because Samd21Rtc::begin() runs GCLK_RTC at 32.768 kHz rather than
  // 1024 Hz. See Samd21Rtc.h.
  uint32_t millis() const override;

private:
  const Samd21Rtc &_rtc;
};
