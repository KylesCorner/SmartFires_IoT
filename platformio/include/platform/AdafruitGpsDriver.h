#pragma once

#include "drivers/IGpsDriver.h"

#include <Adafruit_GPS.h>
#include <Wire.h>

class AdafruitGpsDriver final : public IGpsDriver {
public:
  explicit AdafruitGpsDriver(TwoWire &wire = Wire);

  bool begin(uint8_t address) override;
  bool poll() override;
  bool read(Data &out) override;

private:
  Adafruit_GPS _gps;
  bool _begun = false;
};
