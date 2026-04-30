#pragma once

#include <Arduino.h>

#include "sensors/ITriggerSensor.h"

class FakeTriggerSensor : public ITriggerSensor {
public:
  bool service() override {
    serviceCount++;
    return serviceOk;
  }

  bool ready() const override {
    return readyValue;
  }

  bool sample() override {
    sampleCount++;
    return sampleOk;
  }

  const Reading &triggerReading() const override {
    return readingValue;
  }

  void setReading(float tempC, float humidityPct, bool valid = true) {
    readingValue.tempC = tempC;
    readingValue.humidityPct = humidityPct;
    readingValue.valid = valid;
  }

  bool serviceOk = true;
  bool sampleOk = true;
  bool readyValue = true;

  Reading readingValue;

  uint16_t serviceCount = 0;
  uint16_t sampleCount = 0;
};
