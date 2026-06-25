// ---
// description: Helper functions for naming CMD_CALIBRATE/CMD_RESET types and statuses, and for logging CmdAckPayload summaries.
// role: implementation
// ---
#pragma once

#include "telemetry/BinaryPacket.h"

#include <stdint.h>

namespace CalibrationDebug {

const char *cmdTypeName(uint8_t cmdType);
const char *statusName(uint8_t status);

void logCmdAckSummary(const BinaryPacket::CmdAckPayload &ack,
                      uint8_t nodeId,
                      uint8_t seq,
                      const char *src);

} // namespace CalibrationDebug
