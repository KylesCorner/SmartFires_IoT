#pragma once

#include "drivers/ISht31Driver.h"

#include <math.h>
#include <stdint.h>

class FakeSht31Driver final : public ISht31Driver {
public:
  bool beginOk = true;
  uint8_t lastBeginAddress = 0;

  float tempC = 22.5f;
  float humidityPct = 40.0f;

  bool beginCalled = false;
  uint32_t beginCount = 0;
  uint32_t tempReadCount = 0;
  uint32_t humidityReadCount = 0;

  bool begin(uint8_t address) override {
    beginCalled = true;
    beginCount++;
    lastBeginAddress = address;
    return beginOk;
  }

  float readTemperatureC() override {
    tempReadCount++;
    return tempC;
  }

  float readHumidityPct() override {
    humidityReadCount++;
    return humidityPct;
  }
};
