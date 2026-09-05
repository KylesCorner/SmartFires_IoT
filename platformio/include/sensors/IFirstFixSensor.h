// include/sensors/IFirstFixSensor.h
#pragma once

#include "interfaces/ISensor.h"

class IFirstFixSensor : public ISensor {
public:
  virtual bool hasFix() const = 0;
};