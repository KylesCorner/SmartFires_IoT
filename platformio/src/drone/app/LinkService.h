#pragma once

#include <stdint.h>
#include "DroneContext.h"
#include "DroneState.h"
#include "IClock.h"
#include "TelemetryService.h"
#include "shared/BinaryPacket.h"

class LinkService {
public:
  LinkService(DroneContext& ctx, AppState& state, IClock& clock, TelemetryService& telemetry)
    : _ctx(ctx), _state(state), _clock(clock), _telemetry(telemetry) {}

  void update();
  void maybeSendTelemetry(uint8_t nodeId);
  void handleAckTimeout();

private:
  BinaryPacket::FullStatePayload buildPayload(uint32_t now) const;
  void sendBundleFrame(uint8_t nodeId, uint32_t seq);

  DroneContext&     _ctx;
  AppState&         _state;
  IClock&           _clock;
  TelemetryService& _telemetry;

  // Session time sync state.
  // session_time = millis() + _sessionTimeOffset  (offset is negative until first sync)
  int64_t  _sessionTimeOffset = 0;
  uint32_t _lastSyncSessionId = 0;
  bool     _hasSynced         = false;

  // Bundle accumulator: one reference + kBundleMaxDeltas delta samples.
  static constexpr uint8_t kBundleSamples = BinaryPacket::kBundleMaxDeltas + 1;
  BinaryPacket::FullStatePayload _sampleBuf[kBundleSamples];
  uint8_t _sampleCount = 0;
};
