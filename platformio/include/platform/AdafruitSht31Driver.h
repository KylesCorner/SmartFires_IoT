#pragma once

#include "drivers/ISht31Driver.h"

#include <Adafruit_SHT31.h>

class AdafruitSht31Driver final : public ISht31Driver {
public:
  bool begin(uint8_t address) override;
  float readTemperatureC() override;
  float readHumidityPct() override;

private:
  Adafruit_SHT31 _sht31;
};
