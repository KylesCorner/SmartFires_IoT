#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

#include <stdio.h>
#include <string.h>

class FakeSensor final : public ISensor {
public:
  struct Reading {
    bool valid = false;
    uint32_t sampleCount = 0;
    uint32_t timestampMs = 0;
  };

  FakeSensor(const char *sensorName, SensorDutyClass duty, IClock &clockRef)
      : _name(sensorName), _duty(duty), _clock(clockRef) {}

  uint32_t wakeDelayMs = 0;
  bool beginShouldPass = true;
  bool wakeShouldPass = true;
  bool sleepShouldPass = true;
  bool sampleShouldPass = true;

  bool beginCalled = false;
  bool wakeCalled = false;
  bool sleepCalled = false;
  bool serviceCalled = false;
  bool sampleCalled = false;

  uint32_t beginCount = 0;
  uint32_t wakeCount = 0;
  uint32_t sleepCount = 0;
  uint32_t serviceCount = 0;
  uint32_t sampleCount = 0;

  const char *name() const override {
    return _name;
  }

  bool begin() override {
    beginCalled = true;
    beginCount++;

    if (!beginShouldPass) {
      _healthy = false;
      _state = SensorPowerState::Error;
      return false;
    }

    _healthy = true;

    if (_duty == SensorDutyClass::AlwaysOn) {
      _state = SensorPowerState::Ready;
    } else {
      _state = SensorPowerState::Sleeping;
    }

    return true;
  }

  bool wake() override {
    wakeCalled = true;
    wakeCount++;

    if (!_healthy || !wakeShouldPass) {
      _state = SensorPowerState::Error;
      _healthy = false;
      return false;
    }

    _wakeStartMs = _clock.millis();
    _state = SensorPowerState::Waking;
    return true;
  }

  bool sleep() override {
    sleepCalled = true;
    sleepCount++;

    if (!_healthy || !sleepShouldPass) {
      _state = SensorPowerState::Error;
      _healthy = false;
      return false;
    }

    if (_duty == SensorDutyClass::AlwaysOn) {
      _state = SensorPowerState::Ready;
    } else {
      _state = SensorPowerState::Sleeping;
    }

    return true;
  }

  bool service() override {
    serviceCalled = true;
    serviceCount++;

    if (!_healthy) {
      _state = SensorPowerState::Error;
      return false;
    }

    if (_state == SensorPowerState::Waking &&
        _clock.millis() - _wakeStartMs >= wakeDelayMs) {
      _state = SensorPowerState::Ready;
    }

    return _state == SensorPowerState::Ready;
  }

  bool sample() override {
    sampleCalled = true;
    sampleCount++;

    if (!_healthy || _state != SensorPowerState::Ready || !sampleShouldPass) {
      _reading.valid = false;
      return false;
    }

    _reading.valid = true;
    _reading.sampleCount++;
    _reading.timestampMs = _clock.millis();
    return true;
  }

  bool ready() const override {
    return _healthy && _state == SensorPowerState::Ready;
  }

  bool healthy() const override {
    return _healthy;
  }

  SensorPowerState powerState() const override {
    return _state;
  }

  SensorDutyClass dutyClass() const override {
    return _duty;
  }

  const Reading &reading() const {
    return _reading;
  }

  const void *readingData() const override {
    return &_reading;
  }

  size_t readingSize() const override {
    return sizeof(Reading);
  }

  size_t writeTelemetry(char *out, size_t maxLen) const override {
    if (!out || maxLen == 0) {
      return 0;
    }

    const int n = snprintf(out, maxLen,
                           "%s,valid=%u,samples=%lu,t_ms=%lu",
                           _name,
                           _reading.valid ? 1 : 0,
                           static_cast<unsigned long>(_reading.sampleCount),
                           static_cast<unsigned long>(_reading.timestampMs));

    if (n < 0) {
      return 0;
    }

    if (static_cast<size_t>(n) >= maxLen) {
      return maxLen - 1;
    }

    return static_cast<size_t>(n);
  }

private:
  const char *_name;
  SensorDutyClass _duty;
  IClock &_clock;

  SensorPowerState _state = SensorPowerState::Off;
  bool _healthy = false;
  uint32_t _wakeStartMs = 0;

  Reading _reading;
};
