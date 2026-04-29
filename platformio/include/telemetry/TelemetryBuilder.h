#pragma once

#include "interfaces/ISensor.h"
#include "power/BatteryMonitor.h"
#include "telemetry/TelemetryFrame.h"

#include <stddef.h>
#include <stdint.h>

class TelemetryBuilder {
public:
  struct Config {
    uint8_t nodeId = 1;
    bool includeBattery = true;
  };

  explicit TelemetryBuilder(const Config &cfg);

  bool build(TelemetryFrame &frame,
             ISensor **sensors,
             size_t sensorCount,
             const BatteryMonitor *battery) const;

private:
  Config _cfg;

  bool append(char *out, size_t maxLen, size_t &len, const char *text) const;
};
