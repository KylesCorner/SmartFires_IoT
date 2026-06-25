// ---
// description: Hardware abstraction for the SPS30 particulate-matter sensor driver.
// role: interface
// ---
#pragma once
#include <stdint.h>

class ISps30Driver {
public:
  struct Data {
    float pm1_0 = 0.0f;
    float pm2_5 = 0.0f;
    float pm4_0 = 0.0f;
    float pm10_0 = 0.0f;
    bool valid = false;
  };

  virtual ~ISps30Driver() = default;

  virtual bool begin() = 0;
  virtual bool startMeasurement() = 0;
  virtual bool stopMeasurement() = 0;
  virtual bool read(Data &out) = 0;
};
