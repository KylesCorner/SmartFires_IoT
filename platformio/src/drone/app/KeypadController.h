#pragma once

#include "DroneContext.h"
#include "DroneState.h"

class KeypadController {
public:
  KeypadController(DroneContext& ctx, AppState& state)
    : _ctx(ctx), _state(state) {}

  void update();

private:
  DroneContext& _ctx;
  AppState& _state;
};
