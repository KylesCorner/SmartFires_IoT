#pragma once

#include <stdint.h>

enum class GpsPowerMode {
  FullPowerContinuous,
  Standby,
  Backup,
  PeriodicStandby,
  PeriodicBackup,
  AlwaysLocateStandby,
  AlwaysLocateBackup,
};

struct GpsPeriodicConfig {
  uint32_t runTimeMs = 0;
  uint32_t sleepTimeMs = 0;
  uint32_t secondRunTimeMs = 0;
  uint32_t secondSleepTimeMs = 0;
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

  // New:
  //
  // Driver should perform the strongest reset available.
  //
  // If no RST pin is wired yet, this can safely be implemented as:
  //   return true;
  //
  // Later, your hardware driver can pulse PA1010D RST low, wait for boot,
  // and then return true/false depending on whether the reset operation worked.
  virtual bool reset() = 0;

  virtual bool poll() = 0;
  virtual bool read(Data &out) = 0;

  virtual bool enterFullPower() = 0;
  virtual bool enterStandby() = 0;
  virtual bool enterBackup() = 0;
  virtual bool wakeFromBackup() = 0;

  virtual bool enterPeriodicStandby(const GpsPeriodicConfig &cfg) = 0;
  virtual bool enterPeriodicBackup(const GpsPeriodicConfig &cfg) = 0;
};
