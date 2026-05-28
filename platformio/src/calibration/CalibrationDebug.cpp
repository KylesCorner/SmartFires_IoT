#include "calibration/CalibrationDebug.h"

#include "logging/DebugLogger.h"

namespace CalibrationDebug {

const char *cmdTypeName(uint8_t cmdType) {
  switch (cmdType) {
  case BinaryPacket::PKT_CMD_CALIBRATE:
    return "CMD_CALIBRATE";
  case BinaryPacket::PKT_CMD_RESET:
    return "CMD_RESET";
  default:
    return "UNKNOWN_CMD";
  }
}

const char *statusName(uint8_t status) {
  switch (status) {
  case 0x00:
    return "SUCCESS";
  case 0x01:
    return "LOW_SAMPLE_COUNT";
  case 0x02:
    return "ERROR";
  default:
    return "UNKNOWN_STATUS";
  }
}

void logCmdAckSummary(const BinaryPacket::CmdAckPayload &ack,
                      uint8_t nodeId,
                      uint8_t seq,
                      const char *src) {
  LOG_INFO(src ? src : "calib",
           "cmd_ack node=%u seq=%u cmd=%s status=%s uid_hash=0x%08lX",
           static_cast<unsigned int>(nodeId),
           static_cast<unsigned int>(seq),
           cmdTypeName(ack.cmd_type),
           statusName(ack.status),
           static_cast<unsigned long>(ack.uid_hash));
}

} // namespace CalibrationDebug
