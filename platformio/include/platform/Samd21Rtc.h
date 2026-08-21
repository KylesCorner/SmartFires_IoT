// ---
// description: Sole owner of the SAMD21 RTC peripheral in MODE0 — a free-running 32-bit COUNT at 1024 Hz that never stops, shared by the node's live timebase (Samd21RtcClock) and its MCU-standby driver (Samd21RtcSleep).
// role: implementation
// ---
#pragma once

#include <RTCZero.h>
#include <stdint.h>

// One counter, two consumers. The node's awake time and its asleep time used
// to be measured by different clock trees — the Arduino core's millis() while
// running, the RTC only around standby — with the gap patched in afterwards.
// This class makes both read the same register, so there is no gap to patch.
//
// RTCZero is kept for what it already does well: XOSC32K bring-up, NVIC setup,
// the RTC_Handler vector, and standby entry. begin() then re-drives the
// peripheral itself from RTCZero's MODE2 calendar into MODE0, on its own clock.
//
// Clock tree: XOSC32K (32.768 kHz) → GCLK generator 4, undivided → RTC MODE0
// with PRESCALER DIV32 → COUNT increments at 1024 Hz. RTCZero instead divides
// generator 2 by 32 and runs the prescaler at DIV1, landing COUNT at the same
// 1024 Hz but leaving GCLK_RTC itself at 1024 Hz — and read synchronization
// costs a handful of *GCLK_RTC* cycles, so a COUNT read would block for
// milliseconds. Moving the divide out of the generator and into the RTC
// prescaler keeps the tick rate (and therefore all of Samd21RtcTicks' math)
// identical while cutting a synchronized read to ~150 us, which is what makes
// count() affordable on the millis() hot path. The dedicated generator matters
// for a second reason: generator 2 is shared with the watchdog, so retuning it
// would rescale every watchdog timeout too.
class Samd21Rtc {
public:
  // Call once, as early in setup() as possible: until this runs, COUNT is not
  // ticking and Samd21RtcClock::millis() has nothing to read.
  void begin();

  // Current free-running tick count. Costs a read request plus a
  // synchronization wait (~150 us); the resulting fixed latency cancels in
  // start/end deltas. Not interrupt-safe — the READREQ handshake is stateful,
  // so an ISR reading COUNT could corrupt an in-flight foreground read.
  uint32_t count() const;

  // CMP0 compare alarm, used by Samd21RtcSleep to wake from standby.
  void armCompare(uint32_t compareTicks);
  void disarmCompare();

  void standby();

private:
  RTCZero _rtc;

  static void onAlarm();
  static void waitForSync();
};
