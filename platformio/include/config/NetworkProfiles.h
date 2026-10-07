// ---
// description: Compile-time network profiles whose radio, TDMA, reliability, and node/base cadence values must move together.
// role: config
// docs: [bandwidth-scaling, duty-cycling, lora-vs-lorawan, packet-reliability, software-design, tdma-protocol, tunable-parameters]
// ---
#pragma once

#include "radio/LoRaAirtime.h"

#include <stdint.h>

namespace NetworkProfiles {

enum class ProfileId : uint8_t {
  Sf7 = 7,
  Sf9 = 9,
  Sf10 = 10,
  Sf12 = 12,
};

// Data only. Board pins, wire-format ceilings, sensor electrical limits, and
// calibration values deliberately remain in their existing domain headers.
struct NetworkProfile {
  ProfileId id;

  uint8_t spreadingFactor;
  uint32_t bandwidthHz;
  uint8_t codingRateDenominator;
  uint8_t preambleSymbols;
  bool explicitHeader;
  bool payloadCrc;
  bool lowDataRateOptimization;
  int16_t snrDemodFloorDbX10;

  uint8_t totalSlots;
  uint32_t slotWidthMs;
  uint32_t guardMs;
  uint32_t syncStaleMs;
  uint32_t rxWakeAheadMs;

  uint8_t maxBundleDeltas;
  uint16_t maxOperationalApplicationBytes;
  uint16_t maxBundleAirtimeMs;
  uint16_t txCompletionMarginMs;

  uint32_t continuousSamplePeriodMs;
  uint32_t timedSamplePeriodMs;
  uint32_t statusIntervalMs;

  bool useAppAckSummary;
  uint8_t reliabilityMaxAttempts;
  uint32_t retryWaitMs;
  uint32_t reliabilityMaxAgeMs;
  uint32_t awakenIntervalMs;
  uint32_t periodicTimeSyncMs;
  uint32_t commandAckTimeoutMs;

  uint32_t maxTxDrainBeforeStandbyMs;
  uint32_t timedActiveSampleMs;
  uint32_t timedCyclePeriodMs;
};

constexpr uint8_t kWireBundleMaxDeltas = 14;
constexpr uint16_t kBundleFixedBytes = 27;
constexpr uint16_t kBundleBytesPerDelta = 12;

constexpr uint32_t framePeriodMs(const NetworkProfile &profile) {
  return static_cast<uint32_t>(profile.totalSlots) * profile.slotWidthMs;
}

constexpr uint32_t samplesPerBundle(const NetworkProfile &profile) {
  return static_cast<uint32_t>(profile.maxBundleDeltas) + 1u;
}

// SX1276 RegModemConfig1 bandwidth/coding-rate/header bits. Keeping this
// derived from the selected profile prevents the main radio and passive
// sniffer from drifting onto different modem tuples.
constexpr uint8_t sx127xBandwidthBits(uint32_t bandwidthHz) {
  return bandwidthHz == 125000u ? 0x70u
       : bandwidthHz == 250000u ? 0x80u
                                : 0xFFu;
}

constexpr uint8_t sx127xModemConfig1(const NetworkProfile &profile) {
  return static_cast<uint8_t>(
      sx127xBandwidthBits(profile.bandwidthHz) |
      ((profile.codingRateDenominator - 4u) << 1u) |
      (profile.explicitHeader ? 0u : 1u));
}

constexpr uint16_t bundleApplicationBytes(uint8_t deltas) {
  return static_cast<uint16_t>(kBundleFixedBytes +
                               kBundleBytesPerDelta * deltas);
}

constexpr LoRaAirtime::Modem modem(const NetworkProfile &profile) {
  return LoRaAirtime::Modem{profile.spreadingFactor,
                            profile.bandwidthHz,
                            profile.codingRateDenominator,
                            profile.preambleSymbols,
                            profile.explicitHeader,
                            profile.payloadCrc,
                            profile.lowDataRateOptimization};
}

constexpr uint32_t applicationAirtimeMs(const NetworkProfile &profile,
                                        uint16_t applicationBytes) {
  return LoRaAirtime::applicationMilliseconds(applicationBytes, modem(profile));
}

constexpr uint32_t maxBundleAirtimeMs(const NetworkProfile &profile) {
  return applicationAirtimeMs(profile,
                              profile.maxOperationalApplicationBytes);
}

constexpr bool requiresLowDataRateOptimization(
    const NetworkProfile &profile) {
  return LoRaAirtime::ceilDiv(
             (1u << profile.spreadingFactor) * 1000000u,
             profile.bandwidthHz) > 16000u;
}

// Conservative queue-service model: one frame may be serviced per node slot.
// Count generated BUNDLE and STATUS frames against that opportunity and round
// each contribution upward. Profiles reserve at least 10% for retries/control.
constexpr uint32_t offeredLoadPermille(const NetworkProfile &profile) {
  const uint32_t bundleIntervalMs =
      samplesPerBundle(profile) * profile.continuousSamplePeriodMs;
  return LoRaAirtime::ceilDiv(framePeriodMs(profile) * 1000u,
                             bundleIntervalMs) +
         LoRaAirtime::ceilDiv(framePeriodMs(profile) * 1000u,
                             profile.statusIntervalMs);
}

constexpr uint32_t fingerprintMix(uint32_t hash, uint32_t value) {
  for (uint8_t shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<uint8_t>(value >> shift);
    hash *= 16777619u;
  }
  return hash;
}

// Stable FNV-1a fingerprint over the fields that affect over-air
// compatibility, scheduling, offered load, or deployment interpretation.
constexpr uint32_t fingerprint(const NetworkProfile &profile) {
  uint32_t hash = 2166136261u;
  hash = fingerprintMix(hash, static_cast<uint8_t>(profile.id));
  hash = fingerprintMix(hash, profile.spreadingFactor);
  hash = fingerprintMix(hash, profile.bandwidthHz);
  hash = fingerprintMix(hash, profile.codingRateDenominator);
  hash = fingerprintMix(hash, profile.preambleSymbols);
  hash = fingerprintMix(hash, profile.explicitHeader ? 1u : 0u);
  hash = fingerprintMix(hash, profile.payloadCrc ? 1u : 0u);
  hash = fingerprintMix(hash, profile.lowDataRateOptimization ? 1u : 0u);
  hash = fingerprintMix(hash,
                        static_cast<uint16_t>(profile.snrDemodFloorDbX10));
  hash = fingerprintMix(hash, profile.totalSlots);
  hash = fingerprintMix(hash, profile.slotWidthMs);
  hash = fingerprintMix(hash, profile.guardMs);
  hash = fingerprintMix(hash, profile.syncStaleMs);
  hash = fingerprintMix(hash, profile.rxWakeAheadMs);
  hash = fingerprintMix(hash, profile.maxBundleDeltas);
  hash = fingerprintMix(hash, profile.maxOperationalApplicationBytes);
  hash = fingerprintMix(hash, profile.maxBundleAirtimeMs);
  hash = fingerprintMix(hash, profile.txCompletionMarginMs);
  hash = fingerprintMix(hash, profile.continuousSamplePeriodMs);
  hash = fingerprintMix(hash, profile.timedSamplePeriodMs);
  hash = fingerprintMix(hash, profile.statusIntervalMs);
  hash = fingerprintMix(hash, profile.useAppAckSummary ? 1u : 0u);
  hash = fingerprintMix(hash, profile.reliabilityMaxAttempts);
  hash = fingerprintMix(hash, profile.retryWaitMs);
  hash = fingerprintMix(hash, profile.reliabilityMaxAgeMs);
  hash = fingerprintMix(hash, profile.awakenIntervalMs);
  hash = fingerprintMix(hash, profile.periodicTimeSyncMs);
  hash = fingerprintMix(hash, profile.commandAckTimeoutMs);
  hash = fingerprintMix(hash, profile.maxTxDrainBeforeStandbyMs);
  hash = fingerprintMix(hash, profile.timedActiveSampleMs);
  hash = fingerprintMix(hash, profile.timedCyclePeriodMs);
  return hash;
}

constexpr bool isValid(const NetworkProfile &profile) {
  return static_cast<uint8_t>(profile.id) == profile.spreadingFactor &&
         (profile.spreadingFactor == 7 || profile.spreadingFactor == 9 ||
          profile.spreadingFactor == 10 || profile.spreadingFactor == 12) &&
         profile.bandwidthHz ==
             (profile.spreadingFactor == 12u ? 250000u : 125000u) &&
         sx127xBandwidthBits(profile.bandwidthHz) != 0xFFu &&
         profile.codingRateDenominator == 5u &&
         profile.preambleSymbols == 8u && profile.explicitHeader &&
         profile.payloadCrc &&
         profile.lowDataRateOptimization ==
             requiresLowDataRateOptimization(profile) &&
         profile.snrDemodFloorDbX10 < 0 && profile.totalSlots == 5u &&
         profile.guardMs * 2u < profile.slotWidthMs &&
         profile.maxBundleDeltas <= kWireBundleMaxDeltas &&
         profile.maxOperationalApplicationBytes ==
             bundleApplicationBytes(profile.maxBundleDeltas) &&
         profile.maxBundleAirtimeMs == maxBundleAirtimeMs(profile) &&
         maxBundleAirtimeMs(profile) +
                 static_cast<uint32_t>(profile.txCompletionMarginMs) +
                 2u * profile.guardMs <
             profile.slotWidthMs &&
         profile.continuousSamplePeriodMs > 0u &&
         profile.timedSamplePeriodMs > 0u && profile.statusIntervalMs > 0u &&
         offeredLoadPermille(profile) <= 900u &&
         profile.useAppAckSummary && profile.reliabilityMaxAttempts >= 2u &&
         profile.retryWaitMs >= framePeriodMs(profile) &&
         profile.reliabilityMaxAgeMs >=
             (profile.reliabilityMaxAttempts - 1u) * profile.retryWaitMs &&
         profile.awakenIntervalMs >= framePeriodMs(profile) &&
         profile.periodicTimeSyncMs >= framePeriodMs(profile) &&
         profile.commandAckTimeoutMs > profile.timedCyclePeriodMs &&
         profile.maxTxDrainBeforeStandbyMs >=
             framePeriodMs(profile) + maxBundleAirtimeMs(profile) +
                 profile.txCompletionMarginMs &&
         profile.timedActiveSampleMs %
                 (samplesPerBundle(profile) * profile.timedSamplePeriodMs) ==
             0u &&
         profile.timedCyclePeriodMs > profile.timedActiveSampleMs;
}

// Conservative starting points from MULTI_SF_NETWORK_PROFILES.md. Airtime is
// rounded up to whole milliseconds; it is metadata/validation here, not the
// runtime admission algorithm.
constexpr NetworkProfile kSf7{
    ProfileId::Sf7, 7, 125000, 5, 8, true, true, false, -75,
    5, 900, 20, 1320000, 150,
    14, 195, 318, 22,
    750, 1000, 15000,
    true, 3, 9000, 30000, 5000, 50000, 120000,
    5000, 30000, 75000};

constexpr NetworkProfile kSf9{
    ProfileId::Sf9, 9, 125000, 5, 8, true, true, false, -125,
    5, 1300, 30, 1320000, 200,
    14, 195, 1005, 50,
    750, 1000, 30000,
    true, 3, 13000, 40000, 7000, 65000, 120000,
    8000, 30000, 80000};

constexpr NetworkProfile kSf10{
    ProfileId::Sf10, 10, 125000, 5, 8, true, true, false, -150,
    5, 2200, 50, 1320000, 250,
    14, 195, 1805, 50,
    1000, 1000, 120000,
    true, 3, 22000, 70000, 12000, 110000, 120000,
    15000, 30000, 90000};

constexpr NetworkProfile kSf12{
    ProfileId::Sf12, 12, 250000, 5, 8, true, true, true, -200,
    5, 5000, 100, 1320000, 300,
    7, 111, 2216, 100,
    4000, 4000, 300000,
    true, 3, 50000, 150000, 27000, 125000, 180000,
    30000, 64000, 150000};

static_assert(isValid(kSf7), "invalid SF7 network profile");
static_assert(isValid(kSf9), "invalid SF9 network profile");
static_assert(isValid(kSf10), "invalid SF10 network profile");
static_assert(isValid(kSf12), "invalid SF12 network profile");

constexpr const NetworkProfile &profileForSelector(uint8_t selector) {
  return selector == 7u ? kSf7
       : selector == 9u ? kSf9
       : selector == 10u ? kSf10
                         : kSf12;
}

}  // namespace NetworkProfiles

// A deployed radio build must make the selection explicitly. Native tests and
// non-network power-test firmware use SF7 only as an active-config fixture;
// neither is a deployable radio role, and all four named definitions above
// remain available to native tests.
#if !defined(SMARTFIRES_NETWORK_PROFILE)
#if defined(SMARTFIRES_NATIVE_TEST) || defined(POWER_TEST)
#define SMARTFIRES_NETWORK_PROFILE 7
#else
#error "SMARTFIRES_NETWORK_PROFILE must be 7, 9, 10, or 12"
#endif
#endif

#if SMARTFIRES_NETWORK_PROFILE != 7 && SMARTFIRES_NETWORK_PROFILE != 9 && \
    SMARTFIRES_NETWORK_PROFILE != 10 && SMARTFIRES_NETWORK_PROFILE != 12
#error "Unsupported SMARTFIRES_NETWORK_PROFILE; expected 7, 9, 10, or 12"
#endif

namespace NetworkProfiles {
constexpr uint8_t kSelectedProfileSelector = SMARTFIRES_NETWORK_PROFILE;
constexpr const NetworkProfile &kActiveProfile =
    profileForSelector(kSelectedProfileSelector);
constexpr uint32_t kActiveProfileFingerprint = fingerprint(kActiveProfile);
}  // namespace NetworkProfiles
