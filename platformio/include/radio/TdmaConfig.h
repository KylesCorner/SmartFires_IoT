#pragma once

#include <stddef.h>
#include <stdint.h>

struct TdmaConfig {
  uint8_t nodeId = 1;
  uint8_t baseAddr = 0x01;

  uint8_t numSlots = 4;
  uint32_t slotWidthMs = 900;
  uint32_t guardMs = 20;
  uint32_t syncStaleMs = 1320000;

  uint8_t queueDepth = 4;
  bool enableLinkAck = true;
  uint8_t maxRetries = 1;
  uint16_t ackTimeoutMs = 100;

  bool enableAppReliability = true;
  uint8_t reliabilityWindowDepth = 4;
  uint8_t reliabilityMaxAttempts = 3;
  uint32_t reliabilityMaxAgeMs = 15000;
  uint32_t reliabilityMinRetryGapMs = 2000;

  static TdmaConfig tdmaCfg(uint8_t nodeId_ = 1, uint8_t baseAddr_ = 0x01,
                            uint8_t numSlots_ = 4, uint32_t slotWidthMs_ = 900,
                            uint32_t guardMs_ = 20,
                            uint32_t syncStaleMs_ = 1320000,  // 22 min
                            uint8_t queueDepth_ = 4,
                            bool enableLinkAck_ = true,
                            uint8_t maxRetries_ = 1,
                            uint16_t ackTimeoutMs_ = 100,
                            bool enableAppReliability_ = true,
                            uint8_t reliabilityWindowDepth_ = 4,
                            uint8_t reliabilityMaxAttempts_ = 3,
                            uint32_t reliabilityMaxAgeMs_ = 15000,
                            uint32_t reliabilityMinRetryGapMs_ = 2000) {
    TdmaConfig cfg;
    cfg.nodeId = nodeId_;
    cfg.baseAddr = baseAddr_;
    cfg.numSlots = numSlots_;
    cfg.slotWidthMs = slotWidthMs_;
    cfg.guardMs = guardMs_;
    cfg.syncStaleMs = syncStaleMs_;
    cfg.queueDepth = queueDepth_;
    cfg.enableLinkAck = enableLinkAck_;
    cfg.maxRetries = maxRetries_;
    cfg.ackTimeoutMs = ackTimeoutMs_;
    cfg.enableAppReliability = enableAppReliability_;
    cfg.reliabilityWindowDepth = reliabilityWindowDepth_;
    cfg.reliabilityMaxAttempts = reliabilityMaxAttempts_;
    cfg.reliabilityMaxAgeMs = reliabilityMaxAgeMs_;
    cfg.reliabilityMinRetryGapMs = reliabilityMinRetryGapMs_;
    return cfg;
  }

  // Must be >= BinaryPacket::kMaxBundleLoRaSize (currently 193 bytes).
  static constexpr size_t MaxPayloadLen = 220;
};
