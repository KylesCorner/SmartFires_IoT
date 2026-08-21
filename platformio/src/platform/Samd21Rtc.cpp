// ---
// description: SAMD21 RTC MODE0 bring-up (XOSC32K → undivided GCLK generator 4 → PRESCALER DIV32 → 1024 Hz COUNT32), synchronized COUNT reads, CMP0 alarm arming, and standby entry.
// role: implementation
// ---
#include "platform/Samd21Rtc.h"

#include "logging/DebugLogger.h"

#include <Arduino.h>

namespace {

// GCLK generators 0/1/3 belong to the Arduino core (main, XOSC32K, OSC8M) and
// 2 is the watchdog's — see begin(). 4 is the first free one; the
// GCLK_CLKCTRL_GEN_GCLK4 constant below has to be kept in step with it, since
// CMSIS spells that field as per-generator constants rather than a macro.
constexpr uint32_t kRtcGclkGen = 4;

} // namespace

void Samd21Rtc::onAlarm() {
  // Interrupt existence wakes the SAMD21. RTCZero's RTC_Handler runs this
  // callback and then clears INTFLAG bit 0 — the same bit is MODE2 ALARM0 and
  // MODE0 CMP0, so the library handler works unmodified in COUNT32 mode.
}

void Samd21Rtc::waitForSync() {
  while (RTC->MODE0.STATUS.bit.SYNCBUSY) {
  }
}

uint32_t Samd21Rtc::count() const {
  // COUNT lives in the GCLK_RTC domain; a read request plus sync wait is
  // required for a coherent value. Manual RREQ per read avoids the
  // continuous-read-mode (RCONT) stale-value-after-standby-wake errata.
  RTC->MODE0.READREQ.reg =
      RTC_READREQ_RREQ |
      RTC_READREQ_ADDR(RTC_MODE0_COUNT_OFFSET);
  waitForSync();
  return RTC->MODE0.COUNT.reg;
}

void Samd21Rtc::begin() {
  // RTCZero owns XOSC32K bring-up, NVIC enable for RTC_IRQn, and the
  // RTC_Handler vector. Its GCLK work is deliberately undone below: RTCZero
  // routes the RTC to generator 2, which Adafruit_SleepyDog has already
  // claimed for the watchdog (both happen to want 1024 Hz, so today they
  // coexist by coincidence). Retuning that shared generator to feed this
  // timebase would silently make every watchdog timeout 32x shorter, so the
  // RTC gets a generator of its own instead.
  _rtc.begin(false);
  _rtc.attachInterrupt(onAlarm);

  // Software reset for a clean slate before re-driving the peripheral. GCLK and
  // NVIC state live outside the RTC and survive this. The peripheral stays in
  // reset while its clock is switched over below, so the mode change and the
  // clock change never race each other.
  RTC->MODE0.CTRL.reg = RTC_MODE0_CTRL_SWRST;
  while (RTC->MODE0.STATUS.bit.SYNCBUSY ||
         RTC->MODE0.CTRL.bit.SWRST) {
  }

  // Stand up our own generator at the full crystal rate. Clearing DIVSEL
  // switches GENDIV.DIV from a power-of-two exponent back to a linear divisor,
  // so DIV(1) is a straight pass-through. RUNSTDBY is explicit rather than
  // relying on the RTC's clock request to hold the generator up in standby —
  // "the counter never stops" is the whole premise of this timebase.
  GCLK->GENDIV.reg =
      GCLK_GENDIV_ID(kRtcGclkGen) | GCLK_GENDIV_DIV(1);
  while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {
  }

  GCLK->GENCTRL.reg =
      GCLK_GENCTRL_ID(kRtcGclkGen) |
      GCLK_GENCTRL_GENEN |
      GCLK_GENCTRL_SRC_XOSC32K |
      GCLK_GENCTRL_RUNSTDBY;
  while (GCLK->STATUS.reg & GCLK_STATUS_SYNCBUSY) {
  }

  // Move the RTC's peripheral channel off RTCZero's generator onto ours,
  // disabling the channel before repointing it as the datasheet requires.
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID(RTC_GCLK_ID);
  while (GCLK->STATUS.bit.SYNCBUSY) {
  }

  GCLK->CLKCTRL.reg =
      GCLK_CLKCTRL_ID(RTC_GCLK_ID) |
      GCLK_CLKCTRL_GEN_GCLK4 |
      GCLK_CLKCTRL_CLKEN;
  while (GCLK->STATUS.bit.SYNCBUSY) {
  }

  // Free-running 32-bit counter, one tick per 32 GCLK_RTC edges = 1024 Hz.
  RTC->MODE0.CTRL.reg =
      RTC_MODE0_CTRL_MODE_COUNT32 |
      RTC_MODE0_CTRL_PRESCALER_DIV32;
  waitForSync();

  RTC->MODE0.CTRL.reg |= RTC_MODE0_CTRL_ENABLE;
  waitForSync();

  LOG_INFO("rtc",
           "rtc_begin_ok mode0_count32=1 gclk_gen=%u gclk_hz=32768 "
           "prescaler=32 tick_hz=1024",
           static_cast<unsigned int>(kRtcGclkGen));
}

void Samd21Rtc::armCompare(uint32_t compareTicks) {
  RTC->MODE0.INTFLAG.reg = RTC_MODE0_INTFLAG_CMP0;
  RTC->MODE0.COMP[0].reg = compareTicks;
  waitForSync();
  RTC->MODE0.INTENSET.reg = RTC_MODE0_INTENSET_CMP0;
}

void Samd21Rtc::disarmCompare() {
  RTC->MODE0.INTENCLR.reg = RTC_MODE0_INTENCLR_CMP0;
}

void Samd21Rtc::standby() {
  _rtc.standbyMode();
}
