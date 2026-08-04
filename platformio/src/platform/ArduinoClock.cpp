#include "platform/ArduinoClock.h"

#include <Arduino.h>

uint32_t ArduinoClock::millis() const {
  return ::millis() + _sleepOffsetMs;
}

void ArduinoClock::compensateForSleep(
    uint32_t elapsedMs) {
  _sleepOffsetMs += elapsedMs;
}
