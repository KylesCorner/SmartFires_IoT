// ---
// description: Arms the RTC MODE0 CMP0 alarm, parks the SAMD21 in standby until the requested duration has actually elapsed on the counter, and reports the measured elapsed time.
// role: implementation
// ---
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
    Samd21Rtc &rtc)
    : _rtc(rtc) {}

uint32_t Samd21RtcSleep::sleepFor(
    uint32_t requestedMs) {
  if (requestedMs == 0) {
    return 0;
  }

  const uint32_t targetTicks = msToTicks(requestedMs);
  const uint32_t startTicks = _rtc.count();

  _rtc.armCompare(startTicks + targetTicks);

  LOG_INFO(
      "sleep",
      "standby_enter requested_ms=%lu "
      "start_ticks=%lu target_ticks=%lu",
      static_cast<unsigned long>(requestedMs),
      static_cast<unsigned long>(startTicks),
      static_cast<unsigned long>(targetTicks));

  // The configured watchdog cannot span the current Timed standby interval.
  Watchdog.disable();

  Serial.flush();

#if defined(ARDUINO_ARCH_SAMD)
  USBDevice.detach();
#endif

  // An unexpected interrupt may wake the CPU before the RTC compare.
  // Re-enter standby until the requested time has elapsed.
  do {
    _rtc.standby();
  } while (tickDelta(startTicks, _rtc.count()) < targetTicks);

#if defined(ARDUINO_ARCH_SAMD)
  USBDevice.attach();
#endif

  _rtc.disarmCompare();

  const uint32_t endTicks = _rtc.count();
  const uint32_t elapsedMs =
      ticksToMs(tickDelta(startTicks, endTicks));

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
