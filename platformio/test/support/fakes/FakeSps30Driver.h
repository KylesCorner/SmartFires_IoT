#pragma once

#include "drivers/ISps30Driver.h"

#include <stdint.h>

class FakeSps30Driver final : public ISps30Driver {
public:
  bool beginOk = true;
  bool startMeasurementOk = true;
  bool stopMeasurementOk = true;
  bool readOk = true;

  uint32_t beginCount = 0;
  uint32_t startMeasurementCount = 0;
  uint32_t stopMeasurementCount = 0;
  uint32_t readCount = 0;

  Data data;

  bool begin() override {
    beginCount++;
    return beginOk;
  }

  bool startMeasurement() override {
    startMeasurementCount++;
    return startMeasurementOk;
  }

  bool stopMeasurement() override {
    stopMeasurementCount++;
    return stopMeasurementOk;
  }

  bool read(Data &out) override {
    readCount++;

    if (!readOk) {
      return false;
    }

    out = data;
    return true;
  }

  void setReading(float pm1_0, float pm2_5, float pm4_0, float pm10_0,
                  bool valid = true) {
    data.pm1_0 = pm1_0;
    data.pm2_5 = pm2_5;
    data.pm4_0 = pm4_0;
    data.pm10_0 = pm10_0;
    data.valid = valid;
  }
};
