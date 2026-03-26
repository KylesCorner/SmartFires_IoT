#pragma once
#include <Arduino.h>

class IActuator {
public:
  virtual ~IActuator() = default;

  virtual const char* name() const = 0;
  virtual bool begin() = 0;

  // Called frequently; should be non-blocking.
  virtual void update() = 0;

  // Optional health flag (wiring failures are hard to detect on buzzer).
  virtual bool healthy() const = 0;
};