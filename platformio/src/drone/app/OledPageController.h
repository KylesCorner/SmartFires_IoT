#pragma once

#include "DroneContext.h"
#include "DroneState.h"
#include "IClock.h"

class OledPageController {
public:
  OledPageController(DroneContext &ctx, AppState &state, IClock &clock)
      : _ctx(ctx), _state(state), _clock(clock) {}

  void render();

private:
  void renderHome();
  void renderEnv();
  void renderGps();
  void renderImu();
  void renderUart();
  void renderSps();

  const char *systemStateText() const;
  const char *sensorPageHeader(const char *name) const;

  bool sensorsSleeping() const;
  bool sensingDisabled() const;
  bool warmingUp() const;

  DroneContext &_ctx;
  AppState &_state;
  IClock &_clock;
};
