#pragma once

#include "drivers/ISht31Driver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

class Sht31Sensor final : public ISensor {
public:
  struct Config {
    uint8_t address = 0x44;
    uint32_t minSamplePeriodMs = 1000;
    uint32_t wakeDelayMs = 15;
    SensorDutyClass dutyClass = SensorDutyClass::DutyCycled;
  };

  struct Reading {
    float tempC = NAN;
    float humidityPct = NAN;
    bool valid = false;
    uint32_t timestampMs = 0;
  };

  Sht31Sensor(const Config &cfg, ISht31Driver &driver, IClock &clock);

  const char *name() const override;

  bool begin() override;
  bool wake() override;
  bool sleep() override;
  bool service() override;
  bool sample() override;

  bool ready() const override;
  bool healthy() const override;

  SensorPowerState powerState() const override;
  SensorDutyClass dutyClass() const override;

  const Reading &reading() const;

  const void *readingData() const override;
  size_t readingSize() const override;

  size_t writeTelemetry(char *out, size_t maxLen) const override;

private:
  Config _cfg;
  ISht31Driver &_driver;
  IClock &_clock;

  Reading _reading;

  bool _healthy = false;
  SensorPowerState _state = SensorPowerState::Off;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;
};
