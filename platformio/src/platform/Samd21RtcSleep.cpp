#include "platform/Samd21RtcSleep.h"

#include "config/SystemHealthConfig.h"
#include "logging/DebugLogger.h"

#include <Adafruit_SleepyDog.h>
#include <Arduino.h>

Samd21RtcSleep::Samd21RtcSleep(
    ArduinoClock &clock)
    : _clock(clock) {}

void Samd21RtcSleep::onRtcAlarm() {
  // Interrupt existence wakes the SAMD21.
}

void Samd21RtcSleep::begin() {
  _rtc.begin(false);
  _rtc.attachInterrupt(onRtcAlarm);

  LOG_INFO("sleep", "rtc_begin_ok");
}

uint32_t Samd21RtcSleep::sleepFor(
    uint32_t requestedMs) {
  // RTCZero alarms have one-second resolution.
  const uint32_t requestedSeconds =
      requestedMs / 1000UL;

  if (requestedSeconds == 0) {
    return 0;
  }

  const uint32_t startEpoch = _rtc.getEpoch();
  const uint32_t targetEpoch =
      startEpoch + requestedSeconds;

  _rtc.setAlarmEpoch(targetEpoch);
  _rtc.enableAlarm(
      RTCZero::MATCH_YYMMDDHHMMSS);

  LOG_INFO(
      "sleep",
      "standby_enter requested_ms=%lu "
      "start_epoch=%lu target_epoch=%lu",
      static_cast<unsigned long>(requestedMs),
      static_cast<unsigned long>(startEpoch),
      static_cast<unsigned long>(targetEpoch));

  // The normal watchdog cannot span a five-minute standby.
  Watchdog.disable();

  Serial.flush();

#if defined(ARDUINO_ARCH_SAMD)
  USBDevice.detach();
#endif

  // An unexpected interrupt may wake the CPU before the RTC target.
  // Re-enter standby until the requested time has elapsed.
  do {
    _rtc.standbyMode();
  } while (_rtc.getEpoch() < targetEpoch);

#if defined(ARDUINO_ARCH_SAMD)
  USBDevice.attach();
#endif

  _rtc.disableAlarm();

  const uint32_t endEpoch = _rtc.getEpoch();
  const uint32_t elapsedMs =
      (endEpoch - startEpoch) * 1000UL;

  _clock.compensateForSleep(elapsedMs);

  Watchdog.enable(
      SystemHealthConfig::Watchdog::
          kSteadyStateTimeoutMs);

  LOG_INFO(
      "sleep",
      "standby_exit elapsed_ms=%lu "
      "end_epoch=%lu",
      static_cast<unsigned long>(elapsedMs),
      static_cast<unsigned long>(endEpoch));

  return elapsedMs;
}