// ---
// description: MCU standby driven by the SAMD21 RTC's MODE0 CMP0 compare alarm, giving ~1 ms sleep resolution instead of RTCZero's whole-second calendar alarms.
// role: implementation
// ---
#pragma once

#include "platform/IMcuSleep.h"
#include "platform/Samd21Rtc.h"

#include <stdint.h>

// Sleeps against the same free-running counter Samd21RtcClock reads, so
// standby is just a stretch where nothing polls it — the clock carries on by
// itself and needs no compensation afterwards. sleepFor() still reports how
// long it slept, since callers use that to exclude standby from their own
// bookkeeping (radio-off time in the pending-window retry math, for one), not
// to correct the clock.
class Samd21RtcSleep final : public IMcuSleep {
public:
  explicit Samd21RtcSleep(Samd21Rtc &rtc);

  uint32_t sleepFor(
      uint32_t requestedMs) override;

private:
  Samd21Rtc &_rtc;
};
