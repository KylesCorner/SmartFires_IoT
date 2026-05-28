#pragma once

#include <stddef.h>
#include <stdint.h>

enum class TdmaReliabilityMode : uint8_t {
  StrictLinkAck = 0,
  AppLayerAckSummary = 1,
};

inline TdmaReliabilityMode tdmaReliabilityModeFromValue(uint8_t value) {
  switch (value) {
    case static_cast<uint8_t>(TdmaReliabilityMode::AppLayerAckSummary):
      return TdmaReliabilityMode::AppLayerAckSummary;
    case static_cast<uint8_t>(TdmaReliabilityMode::StrictLinkAck):
    default:
      return TdmaReliabilityMode::StrictLinkAck;
  }
}

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
  uint32_t reliabilityFreshTrafficHoldoffMs = 2000;
  TdmaReliabilityMode reliabilityMode = TdmaReliabilityMode::StrictLinkAck;

  // ACK-paced retry gate (APP_ACK_SUMMARY mode only; logic applied in Phase 2).
  // retry_wait_ms = clamp(expectedAckIntervalMs * retryWaitMultiplierPermille / 1000,
  //                       retryWaitMinMs, retryWaitMaxMs)
  uint32_t expectedAckIntervalMs = 4000;          // expected base-side ACK_SUMMARY cadence
  uint16_t retryWaitMultiplierPermille = 2000;     // 2.0x => 8000 ms at 4 s interval
  uint32_t retryWaitMinMs = 4500;                  // floor: one interval + 500 ms jitter margin
  uint32_t retryWaitMaxMs = 10000;                 // ceiling: 2.5 intervals
  bool requireAckSummaryBeforeFirstRetry = false;  // opt-in: gate first retry on observed ACK summary

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
                            uint32_t reliabilityMinRetryGapMs_ = 2000,
                            uint32_t reliabilityFreshTrafficHoldoffMs_ = 2000,
                            TdmaReliabilityMode reliabilityMode_ =
                              TdmaReliabilityMode::StrictLinkAck) {
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
    cfg.reliabilityFreshTrafficHoldoffMs = reliabilityFreshTrafficHoldoffMs_;
    cfg.reliabilityMode = reliabilityMode_;
    return cfg;
  }

  // Must be >= BinaryPacket::kMaxBundleLoRaSize (currently 193 bytes).
  static constexpr size_t MaxPayloadLen = 220;
};
