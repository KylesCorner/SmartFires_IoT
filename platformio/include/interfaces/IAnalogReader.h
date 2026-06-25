// ---
// description: Abstraction for reading a raw analog value from a pin, decoupling sensors from the Arduino analogRead() API.
// role: interface
// ---
#pragma once

#include <stdint.h>

class IAnalogReader {
public:
  virtual ~IAnalogReader() = default;
  virtual int read(uint8_t pin) = 0;
};
