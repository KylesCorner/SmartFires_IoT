#pragma once

#include "drivers/IGpsDriver.h"

#include <Adafruit_GPS.h>
#include <Arduino.h>
#include <Wire.h>

class AdafruitGpsDriver final : public IGpsDriver {
public:
  explicit AdafruitGpsDriver(TwoWire &wire = Wire, uint8_t wakePin = 11);

  bool begin(uint8_t address) override;
  bool poll() override;
  bool read(Data &out) override;

  bool enterStandby() override;
  bool enterBackup() override;
  bool enterPeriodicStandby(const GpsPeriodicConfig &cfg) override;
  bool enterPeriodicBackup(const GpsPeriodicConfig &cfg) override;
  bool enterFullPower() override;
  bool wakeFromBackup() override;
  bool reset() override;
  
  bool enterAlwaysLocateStandby();
  bool enterAlwaysLocateBackup();

private:
  bool sendPmtkPayload(const char *payload);
  bool enterPeriodic(uint8_t type, const GpsPeriodicConfig &cfg);
  Adafruit_GPS _gps;
  uint8_t _wakePin;
  bool _begun = false;
};
