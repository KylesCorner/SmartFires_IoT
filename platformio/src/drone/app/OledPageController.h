#pragma once

#include "DroneContext.h"
#include "DroneState.h"
#include "IClock.h"

class OledPageController {
public:
  OledPageController(DroneContext& ctx, AppState& state, IClock& clock)
    : _ctx(ctx), _state(state), _clock(clock) {}

  void render();

private:
  void renderEnv();
  void renderGps();
  void renderImu();
  void renderUart();
  void renderLora();
  void renderSps();

  DroneContext& _ctx;
  AppState& _state;
  IClock& _clock;
};
