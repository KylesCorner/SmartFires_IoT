// test/support/fakes/FakeGpsDriver.h
#pragma once

#include "drivers/IGpsDriver.h"

#include <stdint.h>

class FakeGpsDriver : public IGpsDriver {
public:
  bool beginOk = true;
  bool pollOk = true;
  bool readOk = true;

  uint8_t lastAddress = 0;
  uint32_t beginCount = 0;
  uint32_t pollCount = 0;
  uint32_t readCount = 0;

  Data data;

  bool enterStandbyOk = true;
  bool enterBackupOk = true;
  bool enterPeriodicStandbyOk = true;
  bool enterPeriodicBackupOk = true;
  bool enterFullPowerOk = true;
  bool wakeFromBackupOk = true;

  uint32_t enterStandbyCount = 0;
  uint32_t enterBackupCount = 0;
  uint32_t enterPeriodicStandbyCount = 0;
  uint32_t enterPeriodicBackupCount = 0;
  uint32_t enterFullPowerCount = 0;
  uint32_t wakeFromBackupCount = 0;

  GpsPeriodicConfig lastPeriodicCfg;

  bool begin(uint8_t address) override {
    beginCount++;
    lastAddress = address;
    return beginOk;
  }

  bool poll() override {
    pollCount++;
    return pollOk;
  }

  bool read(Data &out) override {
    readCount++;

    if (!readOk) {
      return false;
    }

    out = data;
    return true;
  }

  void setFix(float lat, float lon, float alt, uint8_t sats = 7,
              uint8_t fixQuality = 1, uint8_t hour = 12, uint8_t minute = 34,
              uint8_t second = 56) {
    data.fix = true;
    data.fixQuality = fixQuality;
    data.satellites = sats;
    data.latitudeDeg = lat;
    data.longitudeDeg = lon;
    data.altitudeM = alt;
    data.hour = hour;
    data.minute = minute;
    data.second = second;
  }

  void setNoFix(uint8_t hour = 1, uint8_t minute = 2, uint8_t second = 3) {
    data.fix = false;
    data.fixQuality = 0;
    data.satellites = 0;
    data.latitudeDeg = 0.0f;
    data.longitudeDeg = 0.0f;
    data.altitudeM = 0.0f;
    data.hour = hour;
    data.minute = minute;
    data.second = second;
  }

  bool enterStandby() override {
    enterStandbyCount++;
    return enterStandbyOk;
  }

  bool enterBackup() override {
    enterBackupCount++;
    return enterBackupOk;
  }

  bool enterPeriodicStandby(const GpsPeriodicConfig &cfg) override {
    enterPeriodicStandbyCount++;
    lastPeriodicCfg = cfg;
    return enterPeriodicStandbyOk;
  }

  bool enterPeriodicBackup(const GpsPeriodicConfig &cfg) override {
    enterPeriodicBackupCount++;
    lastPeriodicCfg = cfg;
    return enterPeriodicBackupOk;
  }

  bool enterFullPower() override {
    enterFullPowerCount++;
    return enterFullPowerOk;
  }

  bool wakeFromBackup() override {
    wakeFromBackupCount++;
    return wakeFromBackupOk;
  }
};
