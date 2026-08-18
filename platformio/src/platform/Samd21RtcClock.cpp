// ---
// description: Converts the free-running SAMD21 RTC tick count into the millisecond timebase every IClock consumer reads.
// role: implementation
// ---
#include "platform/Samd21RtcClock.h"

#include "platform/Samd21RtcTicks.h"

Samd21RtcClock::Samd21RtcClock(const Samd21Rtc &rtc)
    : _rtc(rtc) {}

uint32_t Samd21RtcClock::millis() const {
  return Samd21RtcTicks::ticksToMs(_rtc.count());
}
