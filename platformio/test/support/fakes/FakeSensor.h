#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <cstdio>
#include "interfaces/ISensor.h"

class FakeSensor : public ISensor {
public:
  struct DummyReading {
    bool valid = true;
    uint32_t sampleCount = 0;
  };

  FakeSensor(const char *sensorName, SensorDutyClass duty)
      : _name(sensorName), _dutyClass(duty) {}

  const char *name() const override {
    return _name;
  }

  SensorDutyClass dutyClass() const override {
    return _dutyClass;
  }

  bool begin() override {
    beginCount++;
    _healthy = beginOk;
    return beginOk;
  }

  bool service() override {
    serviceCount++;
    return serviceOk;
  }

  bool ready() const override {
    return readyValue;
  }

  bool sample() override {
    sampleCount++;

    if (sampleOk && readyValue) {
      _reading.valid = true;
      _reading.sampleCount = sampleCount;
    }

    return sampleOk && readyValue;
  }

  bool sleep() override {
    sleepCount++;

    if (sleepOk) {
      _powerState = SensorPowerState::Sleeping;
      sleeping = true;
    }

    return sleepOk;
  }

  bool wake() override {
    wakeCount++;

    if (wakeOk) {
      _powerState = SensorPowerState::Ready;
      sleeping = false;
    }

    return wakeOk;
  }

  bool healthy() const override {
    return _healthy;
  }

  SensorPowerState powerState() const override {
    return _powerState;
  }

  const void *readingData() const override {
    return &_reading;
  }

  size_t readingSize() const override {
    return sizeof(_reading);
  }

  size_t writeTelemetry(char *out, size_t maxLen) const override {
    if (!out || maxLen == 0) {
      return 0;
    }

    const int n = snprintf(out, maxLen, "%s,samples=%u,ready=%u",
                           _name,
                           static_cast<unsigned>(sampleCount),
                           readyValue ? 1u : 0u);

    if (n < 0) {
      return 0;
    }

    if (static_cast<size_t>(n) >= maxLen) {
      return maxLen - 1;
    }

    return static_cast<size_t>(n);
  }

  void fillSnapshot(SensorSnapshot &snap) const override {
    (void)snap;
  }

  const char *_name;
  SensorDutyClass _dutyClass;

  bool beginOk = true;
  bool serviceOk = true;
  bool sampleOk = true;
  bool sleepOk = true;
  bool wakeOk = true;
  bool readyValue = true;
  bool sleeping = false;

  uint16_t beginCount = 0;
  uint16_t serviceCount = 0;
  uint16_t sampleCount = 0;
  uint16_t sleepCount = 0;
  uint16_t wakeCount = 0;

private:
  bool _healthy = true;
  SensorPowerState _powerState = SensorPowerState::Ready;
  DummyReading _reading;
};
