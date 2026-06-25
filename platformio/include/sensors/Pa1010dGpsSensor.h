// ---
// description: ISensor implementation wrapping the PA1010D GPS driver, managing GPS power modes and fix readings.
// role: implementation
// ---
#pragma once

#include "drivers/IGpsDriver.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

class Pa1010dGpsSensor final : public ISensor {
public:
  struct Config {
    SensorDutyClass dutyClass = SensorDutyClass::AlwaysOn;
    GpsPowerMode powerMode;
    GpsPeriodicConfig periodic;
    uint32_t minSamplePeriodMs;
    uint32_t wakeDelayMs = 0;
    uint8_t address = 0x10;

    //The gps unit is on its own sub-duty cycle. This config says that the main
    //duty cycle has no control over the gps and the gps will perodically cycle
    //itself. The gps needs to have its wake pin (defined in the driver) to
    //backup its routing info for faster fix when awakened

    static Pa1010dGpsSensor::Config make(
        uint32_t runTimeMs = 4000,
        uint32_t sleepTimeMs = 15000,
        uint32_t secondRunTimeMs = 24000,
        uint32_t secondSleepTimeMs = 90000,
        uint32_t minSamplePeriodMs = 1000,
        GpsPowerMode powermode = GpsPowerMode::PeriodicBackup) {
      Pa1010dGpsSensor::Config cfg;
      cfg.minSamplePeriodMs = minSamplePeriodMs;
      cfg.powerMode = powermode;
      cfg.periodic.runTimeMs = runTimeMs;
      cfg.periodic.sleepTimeMs = sleepTimeMs;
      cfg.periodic.secondRunTimeMs = secondRunTimeMs;
      cfg.periodic.secondSleepTimeMs = secondSleepTimeMs;
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
