#pragma once

#include "platform/ArduinoClock.h"
#include "platform/IMcuSleep.h"

#include <RTCZero.h>
#include <stdint.h>

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
};