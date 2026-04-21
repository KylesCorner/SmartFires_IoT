#pragma once
#include "DroneContext.h"

class SensorManager {
public:
  explicit SensorManager(DroneContext& ctx) : _ctx(ctx) {}

  void beginAll();
  void sampleAll();
  void sampleKeypadOnly();

  void sleepAllSensors();
  void wakeAllSensors();

  void printSensorReadings();

private:
  DroneContext& _ctx;
};
