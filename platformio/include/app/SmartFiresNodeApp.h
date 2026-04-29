#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"
#include "radio/RadioService.h"
#include "telemetry/TelemetryBuilder.h"

class SmartFiresNodeApp {
public:
  struct Config {
    bool enableBattery = true;
  };

  SmartFiresNodeApp(const Config &cfg,
                    IClock &clock,
                    DutyCycleController &duty,
                    TelemetryBuilder &telemetry,
                    RadioService &radio,
                    ISensor **sensors,
                    size_t sensorCount,
                    BatteryMonitor *battery);

  bool begin();
  void update();

private:
  Config _cfg;

  IClock &_clock;
  DutyCycleController &_duty;
  TelemetryBuilder &_telemetry;
  RadioService &_radio;

  ISensor **_sensors;
  size_t _sensorCount;

  BatteryMonitor *_battery;

  bool _initialized = false;
};
