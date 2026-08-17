#pragma once

#include "platform/ArduinoClock.h"
#include "platform/IMcuSleep.h"

#include <RTCZero.h>
#include <stdint.h>

// MCU standby driven by the SAMD21 RTC in MODE0 (COUNT32): a
// free-running 32-bit counter at 1024 Hz with a CMP0 compare alarm,
// giving ~1 ms sleep resolution instead of RTCZero's whole-second
// calendar alarms. RTCZero is kept for what it already does well —
// GCLK routing (XOSC32K/32 → 1024 Hz), NVIC setup, the RTC_Handler
// vector, and standby entry — while begin() re-drives the peripheral
// itself into MODE0.
class Samd21RtcSleep final : public IMcuSleep {
public:
  explicit Samd21RtcSleep(ArduinoClock &clock);

  void begin();

  uint32_t sleepFor(
      uint32_t requestedMs) override;

private:
  ArduinoClock &_clock;
  RTCZero _rtc;

  static void onRtcAlarm();

  static uint32_t readCount();
  static void waitForSync();
};
