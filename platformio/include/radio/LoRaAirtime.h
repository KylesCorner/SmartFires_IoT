// ---
// description: Integer-only LoRa airtime calculation used by compile-time profile validation and runtime TX deadline admission.
// role: implementation
// docs: [bandwidth-scaling, tdma-protocol]
// ---
#pragma once

#include <stdint.h>

namespace LoRaAirtime {

struct Modem {
  uint8_t spreadingFactor;
  uint32_t bandwidthHz;
  uint8_t codingRateDenominator;  // 5 means 4/5, 8 means 4/8
  uint16_t preambleSymbols;
  bool explicitHeader;
  bool payloadCrc;
  bool lowDataRateOptimize;
};

constexpr uint32_t ceilDiv(uint32_t numerator, uint32_t denominator) {
  return denominator == 0 ? 0 : (numerator + denominator - 1u) / denominator;
}

// These small helpers intentionally use C++11's single-return constexpr form.
// The SAMD Arduino toolchain injects -std=gnu++11 after project flags in some
// PlatformIO releases, and profile validation must remain a constant
// expression there as well as in the native C++17 tests.
constexpr uint32_t symbolMicroseconds(const Modem &modem) {
  return ceilDiv((1u << modem.spreadingFactor) * 1000000u,
                 modem.bandwidthHz);
}

constexpr int32_t payloadNumerator(uint16_t bytes, const Modem &modem) {
  return 8 * static_cast<int32_t>(bytes) -
         4 * static_cast<int32_t>(modem.spreadingFactor) + 28 +
         (modem.payloadCrc ? 16 : 0) -
         (modem.explicitHeader ? 0 : 20);
}

constexpr uint32_t payloadDenominator(const Modem &modem) {
  return 4u * static_cast<uint32_t>(
                  modem.spreadingFactor -
                  (modem.lowDataRateOptimize ? 2u : 0u));
}

constexpr uint32_t codedBlocks(uint16_t bytes, const Modem &modem) {
  return payloadNumerator(bytes, modem) > 0
             ? ceilDiv(static_cast<uint32_t>(payloadNumerator(bytes, modem)),
                       payloadDenominator(modem))
             : 0u;
}

constexpr uint32_t payloadSymbols(uint16_t bytes, const Modem &modem) {
  return 8u + codedBlocks(bytes, modem) *
                  static_cast<uint32_t>(modem.codingRateDenominator);
}

// Preamble includes the fixed 4.25-symbol tail. Work in quarter-symbols to
// avoid floating point: preamble*4 + 17 + payload*4.
constexpr uint32_t totalQuarterSymbols(uint16_t bytes, const Modem &modem) {
  return static_cast<uint32_t>(modem.preambleSymbols) * 4u + 17u +
         payloadSymbols(bytes, modem) * 4u;
}

// Returns worst-case on-air microseconds for one RadioHead datagram. `bytes`
// is the complete radio payload length, including RadioHead's four-byte
// addressing header. The formula is Semtech's explicit LoRa packet airtime
// equation, evaluated with integers so it is usable in static_asserts.
constexpr uint32_t microseconds(uint16_t bytes, const Modem &modem) {
  return ceilDiv(totalQuarterSymbols(bytes, modem) *
                     symbolMicroseconds(modem),
                 4u);
}

constexpr uint32_t milliseconds(uint16_t bytes, const Modem &modem) {
  return ceilDiv(microseconds(bytes, modem), 1000u);
}

constexpr uint16_t kRadioHeadHeaderBytes = 4;

constexpr uint32_t applicationMilliseconds(uint16_t applicationBytes,
                                           const Modem &modem) {
  return milliseconds(
      static_cast<uint16_t>(applicationBytes + kRadioHeadHeaderBytes), modem);
}

}  // namespace LoRaAirtime
