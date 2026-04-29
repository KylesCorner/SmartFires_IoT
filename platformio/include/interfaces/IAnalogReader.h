#pragma once

#include <stdint.h>

class IAnalogReader {
public:
  virtual ~IAnalogReader() = default;
  virtual int read(uint8_t pin) = 0;
};
