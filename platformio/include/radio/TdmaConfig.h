#pragma once

#include <stddef.h>
#include <stdint.h>

struct TdmaConfig {
  uint8_t nodeId = 1;
  uint8_t baseAddr = 0x01;

  uint8_t numSlots;
  uint32_t slotWidthMs;
  uint32_t guardMs;
  uint32_t syncStaleMs;

  uint8_t queueDepth;
  uint8_t maxRetries;
  uint16_t ackTimeoutMs;

  static TdmaConfig tdmaCfg(uint8_t nodeId_ = 1, uint8_t baseAddr_ = 0x01,
                            uint8_t numSlots_ = 2, uint32_t slotWidthMs_ = 900,
                            uint32_t guardMs_ = 20,
                            uint32_t syncStaleMs_ = 1320000,  // 22 min
                            uint8_t queueDepth_ = 4, uint8_t maxRetries_ = 1,
                            uint16_t ackTimeoutMs_ = 100) {
    TdmaConfig cfg;
    cfg.nodeId = nodeId_;
    cfg.baseAddr = baseAddr_;
    cfg.numSlots = numSlots_;
    cfg.slotWidthMs = slotWidthMs_;
    cfg.guardMs = guardMs_;
    cfg.syncStaleMs = syncStaleMs_;
    cfg.queueDepth = queueDepth_;
    cfg.maxRetries = maxRetries_;
    cfg.ackTimeoutMs = ackTimeoutMs_;
    return cfg;
  }

  static constexpr size_t MaxPayloadLen = 177;
};
