#pragma once

#include <stdint.h>
#include "DroneContext.h"
#include "shared/TelemetryPacket.h"

class TelemetryService {
public:
  static constexpr uint8_t TF_WIND  = 1 << 0;
  static constexpr uint8_t TF_SHT31 = 1 << 1;
  static constexpr uint8_t TF_GPS   = 1 << 2;
  static constexpr uint8_t TF_IMU   = 1 << 3;
  static constexpr uint8_t TF_SPS30 = 1 << 4;

  TelemetryService(DroneContext& ctx) : _ctx(ctx) {}

  TelemetryPacket build(uint32_t seq, uint32_t nowMs) const;

private:
  DroneContext& _ctx;
};
