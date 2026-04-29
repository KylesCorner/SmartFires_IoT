#pragma once

#include <stdint.h>

class ISht31Driver {
public:
  virtual ~ISht31Driver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual float readTemperatureC() = 0;
  virtual float readHumidityPct() = 0;
};
