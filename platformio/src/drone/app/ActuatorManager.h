#pragma once

#include <stddef.h>
#include "DroneContext.h"

class ActuatorManager {
public:
  explicit ActuatorManager(DroneContext& ctx);

  void beginAll();
  void updateAll();

private:
  DroneContext& _ctx;
};
