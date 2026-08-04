#pragma once

#include "interfaces/IClock.h"

#include <stdint.h>

class ArduinoClock final : public IClock {
public:
  uint32_t millis() const override;

  void compensateForSleep(uint32_t elapsedMs);

private:
  uint32_t _sleepOffsetMs = 0;
};