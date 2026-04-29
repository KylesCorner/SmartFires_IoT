#pragma once

#include "drivers/IIcm20948Driver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

class Icm20948Sensor final : public ISensor {
public:
  struct Config {
    uint32_t minSamplePeriodMs = 10000;
    uint32_t wakeDelayMs = 0;
    SensorDutyClass dutyClass = SensorDutyClass::OnDemand;
  };

  struct Reading : public IIcm20948Driver::Data {
    uint32_t timestampMs = 0;
  };

  Icm20948Sensor(const Config &cfg, IIcm20948Driver &driver, IClock &clock);

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
  IIcm20948Driver &_driver;
  IClock &_clock;

  Reading _reading;
  SensorPowerState _state = SensorPowerState::Off;
  bool _healthy = false;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;
};
