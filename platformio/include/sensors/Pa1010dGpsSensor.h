#pragma once

#include "drivers/IGpsDriver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include <cstdint>

class Pa1010dGpsSensor final : public ISensor {
public:
  struct Config {
    uint32_t minSamplePeriodMs;
    uint32_t wakeDelayMs;
    SensorDutyClass dutyClass;
    uint8_t address;

    static Pa1010dGpsSensor::Config
    makeGpsCfg(uint32_t minSamplePeriodMs_ = 100, uint32_t wakeDelayMs_ = 0,
               SensorDutyClass dutyClass_ = SensorDutyClass::AlwaysOn, uint8_t address_=0x10) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs_;
      cfg.wakeDelayMs = wakeDelayMs_;
      cfg.dutyClass = dutyClass_;
      cfg.address = address_;
      return cfg;
    }
  };

  struct Reading : public IGpsDriver::Data {
    bool valid = false;
    uint32_t timestampMs = 0;
  };

  Pa1010dGpsSensor(const Config &cfg, IGpsDriver &driver, IClock &clock);

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
  IGpsDriver &_driver;
  IClock &_clock;

  Reading _reading;
  SensorPowerState _state = SensorPowerState::Off;
  bool _healthy = false;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;
};
