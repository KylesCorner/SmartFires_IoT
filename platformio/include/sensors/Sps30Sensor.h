#pragma once

#include "drivers/ISps30Driver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

class Sps30Sensor final : public ISensor {
public:
  struct Config {
    uint32_t minSamplePeriodMs = 5000;
    uint32_t wakeDelayMs = 8000;
    SensorDutyClass dutyClass = SensorDutyClass::WarmupHeavy;
  };

  struct Reading : public ISps30Driver::Data {
    uint32_t timestampMs = 0;
  };

  Sps30Sensor(const Config &cfg, ISps30Driver &driver, IClock &clock);

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
  ISps30Driver &_driver;
  IClock &_clock;

  Reading _reading;
  SensorPowerState _state = SensorPowerState::Off;
  bool _healthy = false;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;
};
