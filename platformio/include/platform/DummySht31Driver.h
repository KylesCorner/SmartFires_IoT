#pragma once

#include "drivers/ISht31Driver.h"

// Stub ISht31Driver for DUMMY_SENSORS firmware builds.
// Returns fixed non-NaN values so Sht31Sensor and DutyCycleController
// initialise cleanly with no real hardware attached.
class DummySht31Driver final : public ISht31Driver {
public:
    bool  begin(uint8_t)     override { return true; }
    float readTemperatureC() override { return 25.0f; }
    float readHumidityPct()  override { return 50.0f; }
};
