#pragma once

#include "interfaces/IClock.h"

#include <stdint.h>

class ArduinoClock final : public IClock {
public:
  uint32_t millis() const override;
};
