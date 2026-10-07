// ---
// description: Sensor interface extension for devices that report whether their first valid fix has been acquired.
// role: interface
// ---
#pragma once

#include "interfaces/ISensor.h"

class IFirstFixSensor : public ISensor {
public:
  virtual bool hasFix() const = 0;
};
