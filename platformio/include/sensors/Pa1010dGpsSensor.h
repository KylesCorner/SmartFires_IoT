#pragma once

#include "config/SensingConfig.h"
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
    uint8_t address = SensingConfig::Gps::kAddress;

    // Thin wrapper: defaults come from config/SensingConfig.h's Gps
    // namespace. Each factory below corresponds to a distinct, independently
    // tuned power-mode family — see SensingConfig.h's comment on why a
    // couple of the timing values are intentionally shared between two
    // factories rather than collapsed into one.
    static Pa1010dGpsSensor::Config makeGpsCfg(
        uint32_t minSamplePeriodMs_ = SensingConfig::Gps::kContinuousMinSamplePeriodMs,
        uint32_t wakeDelayMs_ = SensingConfig::Gps::kContinuousWakeDelayMs,
        SensorDutyClass dutyClass_ = SensorDutyClass::AlwaysOn,
        uint8_t address_ = SensingConfig::Gps::kAddress,
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
        uint32_t runTimeMs = SensingConfig::Gps::kPeriodicRunTimeMs,
        uint32_t sleepTimeMs = SensingConfig::Gps::kPeriodicSleepTimeMs,
        uint32_t secondRunTimeMs = SensingConfig::Gps::kPeriodicSecondRunTimeMs,
        uint32_t secondSleepTimeMs = SensingConfig::Gps::kPeriodicSecondSleepTimeMs,
        uint32_t minSamplePeriodMs = SensingConfig::Gps::kPeriodicMinSamplePeriodMs,
        uint8_t address = SensingConfig::Gps::kAddress) {
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
        uint32_t runTimeMs = SensingConfig::Gps::kPeriodicRunTimeMs,
        uint32_t sleepTimeMs = SensingConfig::Gps::kPeriodicSleepTimeMs,
        uint32_t secondRunTimeMs = SensingConfig::Gps::kPeriodicSecondRunTimeMs,
        uint32_t secondSleepTimeMs = SensingConfig::Gps::kPeriodicSecondSleepTimeMs,
        uint32_t minSamplePeriodMs = SensingConfig::Gps::kPeriodicMinSamplePeriodMs,
        uint8_t address = SensingConfig::Gps::kAddress) {
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

    static Pa1010dGpsSensor::Config makeAlwaysLocateStandbyCfg(
        uint32_t minSamplePeriodMs = SensingConfig::Gps::kAlwaysLocateMinSamplePeriodMs,
        uint8_t address = SensingConfig::Gps::kAddress) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs;
      cfg.wakeDelayMs = 0;
      cfg.dutyClass = SensorDutyClass::AlwaysOn;
      cfg.address = address;
      cfg.powerMode = GpsPowerMode::AlwaysLocateStandby;
      cfg.periodic = GpsPeriodicConfig{};
      return cfg;
    }

    static Pa1010dGpsSensor::Config makeAlwaysLocateBackupCfg(
        uint32_t minSamplePeriodMs = SensingConfig::Gps::kAlwaysLocateMinSamplePeriodMs,
        uint8_t address = SensingConfig::Gps::kAddress) {
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
  bool reset() override;

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
