#pragma once

#include <stdint.h>

class IIcm20948Driver {
public:
  struct Data {
    float   headingDeg      = 0.0f;
    int16_t headingAccuracy = 0;    // Q12 raw; divide by 4096 for degrees
    bool    headingValid    = false;
    bool    valid           = false;
  };

  virtual ~IIcm20948Driver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual bool read(Data &out) = 0;
};
