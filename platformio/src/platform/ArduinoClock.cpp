// ---
// description: Arduino core millis() passthrough for IClock.
// role: implementation
// ---
#include "platform/ArduinoClock.h"

#include <Arduino.h>

uint32_t ArduinoClock::millis() const {
  return ::millis();
}
