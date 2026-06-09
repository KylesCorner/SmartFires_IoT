#pragma once

#include <stdint.h>

class IBmv080Driver {
public:
  struct Data {
    float pm1_0 = 0.0f;
    float pm2_5 = 0.0f;
    float pm10_0 = 0.0f;
    bool obstructed = false;
    bool valid = false;
  };

  virtual ~IBmv080Driver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual bool startMeasurement() = 0;
  virtual bool stopMeasurement() = 0;
  virtual bool reset() = 0;
  virtual bool read(Data &out) = 0;
};