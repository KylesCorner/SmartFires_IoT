#pragma once

#include "drivers/IBmv080Driver.h"

#include <Arduino.h>
#include <Wire.h>
#include <SparkFun_BMV080_Arduino_Library.h>

class SparkfunBmv080Driver final : public IBmv080Driver {
public:
  explicit SparkfunBmv080Driver(TwoWire &wire = Wire);

  bool begin(uint8_t address) override;
  bool startMeasurement() override;
  bool stopMeasurement() override;
  bool reset() override;
  bool read(Data &out) override;

private:
  TwoWire &_wire;
  SparkFunBMV080 _sensor;
  uint8_t _address = 0x57;
  bool _begun = false;
  bool _measuring = false;
};