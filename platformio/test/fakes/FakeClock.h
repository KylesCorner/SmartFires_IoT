#pragma once

#include "interfaces/IClock.h"

class FakeClock final : public IClock {
public:
  uint32_t nowMs = 0;

  uint32_t millis() const override {
    return nowMs;
  }

  void advance(uint32_t ms) {
    nowMs += ms;
  }

  void set(uint32_t ms) {
    nowMs = ms;
  }
};
