// ---
// description: Tunable TDMA/reliability runtime config struct and reliability-mode enum.
// role: config
// docs: [packet-reliability, tdma-protocol]
// ---
#pragma once

// Shape of the TDMA/reliability runtime config consumed by TdmaClock,
// TdmaTxQueue, and TdmaRadioService.
//
// This struct intentionally carries its own conservative member-default
// values as a safety fallback for an unpopulated `TdmaConfig cfg;` — but
// those defaults are NOT what ships on any real build. The single source of
// truth for the operating values actually used by node firmware is
// config/NetworkConfig.h's nodeTdmaProfile(); see that file (and
// documentation/Current_Architecture/TUNABLE_PARAMETERS.md) before changing
// any tuning value here.

#include <stddef.h>
#include <stdint.h>

enum class TdmaReliabilityMode : uint8_t {
  StrictLinkAck = 0,
  AppLayerAckSummary = 1,
};

// Written as a ternary rather than a switch so it is valid constexpr under
// C++11 (the Arduino SAMD toolchain's default standard).  C++11 constexpr
// functions may only contain a single return statement; switch/if bodies
// require C++14 or later.
constexpr TdmaReliabilityMode tdmaReliabilityModeFromValue(uint8_t value) {
  return (value == static_cast<uint8_t>(TdmaReliabilityMode::AppLayerAckSummary))
      ? TdmaReliabilityMode::AppLayerAckSummary
      : TdmaReliabilityMode::StrictLinkAck;
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

  // ACK-paced retry gate (APP_ACK_SUMMARY mode only).
  // retry_wait_ms = clamp(expectedAckIntervalMs * retryWaitMultiplierPermille / 1000,
  //                       retryWaitMinMs, retryWaitMaxMs)
  uint32_t expectedAckIntervalMs = 4000;          // expected base-side ACK_SUMMARY cadence
  uint16_t retryWaitMultiplierPermille = 2000;     // 2.0x => 8000 ms at 4 s interval
  uint32_t retryWaitMinMs = 4500;                  // floor: one interval + 500 ms jitter margin
  uint32_t retryWaitMaxMs = 10000;                 // ceiling: 2.5 intervals
  bool requireAckSummaryBeforeFirstRetry = false;  // opt-in: gate first retry on observed ACK summary

  // Must be >= BinaryPacket::kMaxBundleLoRaSize (checked via static_assert
  // in config/NetworkConfig.h, which includes BinaryPacket.h — kept out of
  // this header to avoid pulling the wire-format header into every
  // TdmaConfig.h include site).
  static constexpr size_t MaxPayloadLen = 220;
};
