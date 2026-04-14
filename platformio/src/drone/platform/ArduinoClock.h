#pragma once
#include "IClock.h"
#include <Arduino.h>

class ArduinoClock final : public IClock {
public:
  uint32_t millis() const override { return ::millis(); }
};
