#include "platform/Samd21RtcSleep.h"

#include "config/SystemHealthConfig.h"
#include "logging/DebugLogger.h"
#include "platform/Samd21RtcTicks.h"

#include <Adafruit_SleepyDog.h>
#include <Arduino.h>

using Samd21RtcTicks::msToTicks;
using Samd21RtcTicks::tickDelta;
using Samd21RtcTicks::ticksToMs;

Samd21RtcSleep::Samd21RtcSleep(
    ArduinoClock &clock)
    : _clock(clock) {}

void Samd21RtcSleep::onRtcAlarm() {
  // Interrupt existence wakes the SAMD21. RTCZero's RTC_Handler runs
  // this callback and then clears INTFLAG bit 0 — the same bit is
  // MODE2 ALARM0 and MODE0 CMP0, so the library handler works
  // unmodified in COUNT32 mode.
}

void Samd21RtcSleep::waitForSync() {
  while (RTC->MODE0.STATUS.bit.SYNCBUSY) {
  }
}

uint32_t Samd21RtcSleep::readCount() {
  // COUNT lives in the 1024 Hz clock domain; a read request plus sync
  // wait is required for a coherent value. The returned count carries
  // a fixed sync latency (~1-2 ticks), which cancels in start/end
  // deltas. Manual RREQ per read avoids the continuous-read-mode
  // (RCONT) stale-value-after-standby-wake errata.
  RTC->MODE0.READREQ.reg =
      RTC_READREQ_RREQ |
      RTC_READREQ_ADDR(RTC_MODE0_COUNT_OFFSET);
  waitForSync();
  return RTC->MODE0.COUNT.reg;
}

void Samd21RtcSleep::begin() {
  // RTCZero owns GCLK gen 2 setup (XOSC32K / 32 → 1024 Hz into the
  // RTC), NVIC enable for RTC_IRQn, and the RTC_Handler vector.
  _rtc.begin(false);
  _rtc.attachInterrupt(onRtcAlarm);

  // Re-drive the peripheral from RTCZero's MODE2 calendar into MODE0
  // COUNT32: software reset for a clean slate (GCLK and NVIC state are
  // outside the peripheral and survive), then free-running 32-bit
  // counter with no prescaling — one tick per 1024 Hz clock edge.
  RTC->MODE0.CTRL.reg = RTC_MODE0_CTRL_SWRST;
  while (RTC->MODE0.STATUS.bit.SYNCBUSY ||
         RTC->MODE0.CTRL.bit.SWRST) {
  }

  RTC->MODE0.CTRL.reg =
      RTC_MODE0_CTRL_MODE_COUNT32 |
      RTC_MODE0_CTRL_PRESCALER_DIV1;
  waitForSync();

  RTC->MODE0.CTRL.reg |= RTC_MODE0_CTRL_ENABLE;
  waitForSync();

  LOG_INFO("sleep", "rtc_begin_ok mode0_count32=1 tick_hz=1024");
}

uint32_t Samd21RtcSleep::sleepFor(
    uint32_t requestedMs) {
  if (requestedMs == 0) {
    return 0;
  }

  const uint32_t targetTicks = msToTicks(requestedMs);
  const uint32_t startTicks = readCount();

  RTC->MODE0.INTFLAG.reg = RTC_MODE0_INTFLAG_CMP0;
  RTC->MODE0.COMP[0].reg = startTicks + targetTicks;
  waitForSync();
  RTC->MODE0.INTENSET.reg = RTC_MODE0_INTENSET_CMP0;

  LOG_INFO(
      "sleep",
      "standby_enter requested_ms=%lu "
      "start_ticks=%lu target_ticks=%lu",
      static_cast<unsigned long>(requestedMs),
      static_cast<unsigned long>(startTicks),
      static_cast<unsigned long>(targetTicks));

  // The normal watchdog cannot span a five-minute standby.
  Watchdog.disable();

  Serial.flush();

#if defined(ARDUINO_ARCH_SAMD)
  USBDevice.detach();
#endif

  // An unexpected interrupt may wake the CPU before the RTC compare.
  // Re-enter standby until the requested time has elapsed.
  do {
    _rtc.standbyMode();
  } while (tickDelta(startTicks, readCount()) < targetTicks);

#if defined(ARDUINO_ARCH_SAMD)
  USBDevice.attach();
#endif

  RTC->MODE0.INTENCLR.reg = RTC_MODE0_INTENCLR_CMP0;

  const uint32_t endTicks = readCount();
  const uint32_t elapsedMs =
      ticksToMs(tickDelta(startTicks, endTicks));

  _clock.compensateForSleep(elapsedMs);

  Watchdog.enable(
      SystemHealthConfig::Watchdog::
          kSteadyStateTimeoutMs);

  LOG_INFO(
      "sleep",
      "standby_exit elapsed_ms=%lu "
      "end_ticks=%lu",
      static_cast<unsigned long>(elapsedMs),
      static_cast<unsigned long>(endTicks));

  return elapsedMs;
}
