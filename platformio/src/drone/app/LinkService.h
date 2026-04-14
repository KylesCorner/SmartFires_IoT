#pragma once

#include <stdint.h>
#include "DroneContext.h"
#include "DroneState.h"
#include "IClock.h"
#include "TelemetryService.h"

class LinkService {
public:
  LinkService(DroneContext& ctx, AppState& state, IClock& clock, TelemetryService& telemetry)
    : _ctx(ctx), _state(state), _clock(clock), _telemetry(telemetry) {}

  void update();
  void maybeSendTelemetry(uint8_t nodeId);
  void handleAckTimeout();

private:
  void sendTelemetryFrame(uint8_t nodeId, uint32_t seq);

  DroneContext& _ctx;
  AppState& _state;
  IClock& _clock;
  TelemetryService& _telemetry;
};
