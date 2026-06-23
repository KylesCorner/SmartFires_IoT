// ---
// description: Arduino millis()-backed implementation of IClock.
// role: implementation
// ---
#pragma once

#include "interfaces/IClock.h"

#include <stdint.h>

class ArduinoClock final : public IClock {
public:
  uint32_t millis() const override;
};
