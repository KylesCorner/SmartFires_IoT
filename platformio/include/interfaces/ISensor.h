#pragma once

#include "telemetry/SensorSnapshot.h"

#include <stddef.h>
#include <stdint.h>

enum class SensorPowerState : uint8_t { Off, Sleeping, Waking, Ready, Error };

enum class SensorDutyClass : uint8_t {
  AlwaysOn,
  DutyCycled,
  WarmupHeavy,
  OnDemand,
  Perodic,
};

class ISensor {
public:
  virtual ~ISensor() = default;

  virtual const char *name() const = 0;

  virtual bool begin() = 0;
  virtual bool wake() = 0;
  virtual bool sleep() = 0;
  virtual bool service() = 0;
  virtual bool sample() = 0;
  virtual bool reset() {
    (void)sleep();
    return begin();
  }
  virtual bool ready() const = 0;
  virtual bool healthy() const = 0;

  virtual SensorPowerState powerState() const = 0;
  virtual SensorDutyClass dutyClass() const = 0;

  virtual const void *readingData() const = 0;
  virtual size_t readingSize() const = 0;

  virtual size_t writeTelemetry(char *out, size_t maxLen) const = 0;

  virtual void fillSnapshot(SensorSnapshot &snap) const {}
};
