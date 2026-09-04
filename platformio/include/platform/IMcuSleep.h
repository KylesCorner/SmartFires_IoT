// ---
// description: Abstract MCU sleep interface returning the elapsed standby duration.
// role: interface
// ---
#pragma once

#include <stdint.h>

class IMcuSleep {
public:
  virtual ~IMcuSleep() = default;

  // Returns the elapsed sleep duration in milliseconds.
  virtual uint32_t sleepFor(
      uint32_t requestedMs) = 0;
};
