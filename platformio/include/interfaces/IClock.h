#pragma once

#include <stdint.h>

class IClock {
public:
  virtual ~IClock() = default;
  virtual uint32_t millis() const = 0;
};
