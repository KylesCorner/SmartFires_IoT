#pragma once
#include "DroneContext.h"

class SensorManager {
public:
  explicit SensorManager(DroneContext& ctx) : _ctx(ctx) {}

  void beginAll();
  void sampleAll();
  void sampleKeypadOnly();

private:
  DroneContext& _ctx;
};
