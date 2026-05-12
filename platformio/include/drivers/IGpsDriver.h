#pragma once
#include <stdint.h>

enum class GpsPowerMode {
  FullPowerContinuous,
  Standby,
  Backup,
  PeriodicStandby,
  PeriodicBackup,
};

struct GpsPeriodicConfig {
  uint32_t runTimeMs = 4000;
  uint32_t sleepTimeMs = 15000;
  uint32_t secondRunTimeMs = 24000;
  uint32_t secondSleepTimeMs = 90000;
};

class IGpsDriver {
public:
  struct Data {
    bool fix = false;
    uint8_t fixQuality = 0;
    uint8_t satellites = 0;

    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
    float altitudeM = 0.0f;

    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
  };

  virtual ~IGpsDriver() = default;

  virtual bool begin(uint8_t address) = 0;
  virtual bool poll() = 0;
  virtual bool read(Data &out) = 0;

  virtual bool enterStandby() = 0;
  virtual bool enterBackup() = 0;
  virtual bool enterPeriodicStandby(const GpsPeriodicConfig &cfg) = 0;
  virtual bool enterPeriodicBackup(const GpsPeriodicConfig &cfg) = 0;
  virtual bool enterFullPower() = 0;
  virtual bool wakeFromBackup() = 0;
};
