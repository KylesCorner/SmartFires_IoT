#pragma once

#include <Arduino.h>

#include "interfaces/IClock.h"

class FakeClock : public IClock {
public:
  uint32_t nowMs = 0;

  uint32_t millis() const override {
    return nowMs;
  }

  void set(uint32_t ms) {
    nowMs = ms;
  }

  void advance(uint32_t ms) {
    nowMs += ms;
  }
};
