#pragma once

#include "interfaces/IAnalogReader.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

class WindSensorRevC final : public ISensor {
public:
  struct Config {
    uint8_t pinRv = 0;
    uint8_t pinTmp = 1;

    float adcRefVolts = 3.3f;
    uint16_t adcMax = 1023;

    float rvDividerRatio = 1.0f;
    float tmpDividerRatio = 1.0f;

    float zeroWindAdjustmentVolts = 0.2f;

    uint32_t minSamplePeriodMs = 1000;
    uint32_t wakeDelayMs = 0;

    SensorDutyClass dutyClass = SensorDutyClass::DutyCycled;
  };

  struct Reading {
    int rawRv = 0;
    int rawTmp = 0;

    float rvVolts = NAN;
    float tmpVolts = NAN;

    float tempC = NAN;
    float windMps = NAN;

    bool valid = false;
    uint32_t timestampMs = 0;
  };

  WindSensorRevC(const Config &cfg, IAnalogReader &analog, IClock &clock);

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

  const void *readingData() const override;
  size_t readingSize() const override;

  size_t writeTelemetry(char *out, size_t maxLen) const override;

private:
  Config _cfg;
  IAnalogReader &_analog;
  IClock &_clock;

  Reading _reading;

  bool _healthy = false;
  SensorPowerState _state = SensorPowerState::Off;

  uint32_t _wakeStartMs = 0;
  uint32_t _lastSampleMs = 0;

  float adcToVolts(int raw, float dividerRatio) const;
  float estimateTempC(float tmpVolts) const;
  float estimateWindMps(float rvVolts, float tempC) const;
};
