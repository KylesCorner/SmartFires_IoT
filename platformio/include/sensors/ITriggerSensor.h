// include/sensors/ITriggerSensor.h
#pragma once

class ITriggerSensor {
public:
  struct Reading {
    bool valid = false;
    float tempC = 0.0f;
    float humidityPct = 0.0f;
  };

  virtual ~ITriggerSensor() = default;

  virtual bool service() = 0;
  virtual bool ready() const = 0;
  virtual bool sample() = 0;

  virtual const Reading &triggerReading() const = 0;
};
