#pragma once

#include "drivers/ISht31Driver.h"

#include <stdint.h>

class FakeSht31Driver final : public ISht31Driver {
public:
  bool beginOk = true;

  uint8_t lastAddress = 0;
  uint32_t beginCount = 0;
  uint32_t readTempCount = 0;
  uint32_t readHumidityCount = 0;

  float tempC = 22.5f;
  float humidityPct = 45.0f;

  bool begin(uint8_t address) override {
    beginCount++;
    lastAddress = address;
    return beginOk;
  }

  float readTemperatureC() override {
    readTempCount++;
    return tempC;
  }

  float readHumidityPct() override {
    readHumidityCount++;
    return humidityPct;
  }

  void setReading(float tempC_, float humidityPct_) {
    tempC = tempC_;
    humidityPct = humidityPct_;
  }
};
