#pragma once

#include "drivers/ISps30Driver.h"

#include <Arduino.h>
#include <SensirionUartSps30.h>

class SensirionUartSps30Driver final : public ISps30Driver {
public:
  explicit SensirionUartSps30Driver(Stream &serial);

  bool begin() override;
  bool startMeasurement() override;
  bool stopMeasurement() override;
  bool read(Data &out) override;

private:
  static constexpr int16_t NO_ERROR_CODE = 0;

  Stream &_serial;
  SensirionUartSps30 _sensor;
  bool _begun = false;
  bool _measuring = false;
};
