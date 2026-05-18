#pragma once

#include "drivers/IGpsDriver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

class Pa1010dGpsSensor final : public ISensor {
public:
  struct Config {
    SensorDutyClass dutyClass;
    GpsPowerMode powerMode;
    GpsPeriodicConfig periodic;
    uint32_t minSamplePeriodMs;
    uint32_t wakeDelayMs;
    uint8_t address = 0x10;
    static Pa1010dGpsSensor::Config

    makeGpsCfg(uint32_t minSamplePeriodMs_ = 100, uint32_t wakeDelayMs_ = 0,
               SensorDutyClass dutyClass_ = SensorDutyClass::AlwaysOn,
               uint8_t address_ = 0x10,
               GpsPowerMode powerMode_ = GpsPowerMode::FullPowerContinuous) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs_;
      cfg.wakeDelayMs = wakeDelayMs_;
      cfg.dutyClass = dutyClass_;
      cfg.address = address_;
      cfg.powerMode = powerMode_;
      cfg.periodic = GpsPeriodicConfig{};
      return cfg;
    }

    static Pa1010dGpsSensor::Config makePeriodicStandbyCfg(
        uint32_t runTimeMs = 4000, uint32_t sleepTimeMs = 15000,
        uint32_t secondRunTimeMs = 24000, uint32_t secondSleepTimeMs = 90000,
        uint32_t minSamplePeriodMs = 1000, uint8_t address = 0x10) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs;
      cfg.wakeDelayMs = 0;
      cfg.dutyClass = SensorDutyClass::AlwaysOn;
      cfg.address = address;
      cfg.powerMode = GpsPowerMode::PeriodicStandby;
      cfg.periodic.runTimeMs = runTimeMs;
      cfg.periodic.sleepTimeMs = sleepTimeMs;
      cfg.periodic.secondRunTimeMs = secondRunTimeMs;
      cfg.periodic.secondSleepTimeMs = secondSleepTimeMs;
      return cfg;
    }

    static Pa1010dGpsSensor::Config makePeriodicBackupCfg(
        uint32_t runTimeMs = 4000, uint32_t sleepTimeMs = 15000,
        uint32_t secondRunTimeMs = 24000, uint32_t secondSleepTimeMs = 90000,
        uint32_t minSamplePeriodMs = 1000, uint8_t address = 0x10) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs;
      cfg.wakeDelayMs = 0;
      cfg.dutyClass = SensorDutyClass::AlwaysOn;
      cfg.address = address;
      cfg.powerMode = GpsPowerMode::PeriodicBackup;
      cfg.periodic.runTimeMs = runTimeMs;
      cfg.periodic.sleepTimeMs = sleepTimeMs;
      cfg.periodic.secondRunTimeMs = secondRunTimeMs;
      cfg.periodic.secondSleepTimeMs = secondSleepTimeMs;
      return cfg;
    }

    static Pa1010dGpsSensor::Config
    makeAlwaysLocateStandbyCfg(uint32_t minSamplePeriodMs = 1000,
                               uint8_t address = 0x10) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs;
      cfg.wakeDelayMs = 0;
      cfg.dutyClass = SensorDutyClass::AlwaysOn;
      cfg.address = address;
      cfg.powerMode = GpsPowerMode::AlwaysLocateStandby;
      cfg.periodic = GpsPeriodicConfig{};
      return cfg;
    }

    static Pa1010dGpsSensor::Config
    makeAlwaysLocateBackupCfg(uint32_t minSamplePeriodMs = 1000,
                              uint8_t address = 0x10) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs;
      cfg.wakeDelayMs = 0;
      cfg.dutyClass = SensorDutyClass::AlwaysOn;
      cfg.address = address;
      cfg.powerMode = GpsPowerMode::AlwaysLocateBackup;
      cfg.periodic = GpsPeriodicConfig{};
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

  void fillSnapshot(SensorSnapshot &snap) const override;

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
