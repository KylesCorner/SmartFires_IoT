// ---
// description: Abstraction over millis()-based timekeeping so timing-dependent code can be unit tested with a fake clock.
// role: interface
// ---
#pragma once

#include <stdint.h>

class IClock {
public:
  virtual ~IClock() = default;
  virtual uint32_t millis() const = 0;
};
