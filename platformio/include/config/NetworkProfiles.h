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
  // Radio/modem identity. `id` is the over-air profile identity; multiple
  // build selectors may intentionally share it when they use the same modem.
  ProfileId id;
  uint8_t spreadingFactor;          // LoRa SF (7, 9, 10, or 12).
  uint32_t bandwidthHz;             // LoRa bandwidth in Hz.
  uint8_t codingRateDenominator;    // LoRa 4/N denominator; currently 5.
  uint8_t preambleSymbols;          // LoRa preamble length in symbols.
  bool explicitHeader;              // true = explicit LoRa header mode.
  bool payloadCrc;                  // true = SX127x payload CRC enabled.
  bool lowDataRateOptimization;     // SX127x low-data-rate optimization bit.
  int16_t snrDemodFloorDbX10;       // Demodulation floor, dB times ten.

  // TDMA geometry and clock recovery.
  uint8_t totalSlots;               // Base slot plus all node slots.
  uint32_t slotWidthMs;             // Width of each TDMA slot.
  uint32_t guardMs;                 // Quiet time at both slot edges.
  uint32_t syncStaleMs;             // Age at which TIME_SYNC is stale.
  uint32_t rxWakeAheadMs;           // Radio wake lead before base slot 0.

  // BUNDLE sizing and transmit admission.
  uint8_t maxBundleDeltas;          // Samples per BUNDLE minus one.
  uint16_t maxOperationalApplicationBytes;  // 27 + 12 * maxBundleDeltas.
  uint16_t maxBundleAirtimeMs;      // Derived maximum BUNDLE airtime.
  uint16_t txCompletionMarginMs;    // Software/driver completion allowance.

  // Sensing and STATUS cadence. The node application schedules STATUS
  // independently; push() retains a compatibility check for simpler callers.
  uint32_t continuousSamplePeriodMs;  // Continuous-mode sensing interval.
  uint32_t timedSamplePeriodMs;       // Timed-mode active sensing interval.
  uint32_t statusIntervalMs;          // Minimum interval between STATUS packets.

  // Application reliability, discovery, synchronization, and commands.
  bool useAppAckSummary;             // Enable cumulative application ACKs.
  uint8_t reliabilityMaxAttempts;    // Initial send plus retries.
  uint32_t retryWaitMs;              // Minimum age before retransmission.
  uint32_t reliabilityMaxAgeMs;      // Drop pending telemetry after this age.
  uint32_t awakenIntervalMs;         // Unassigned-node AWAKEN cadence.
  uint32_t periodicTimeSyncMs;       // Base TIME_SYNC broadcast cadence.
  uint32_t commandAckTimeoutMs;      // Base wait for node command ACK.

  // Timed duty-cycle geometry.
  uint32_t maxTxDrainBeforeStandbyMs;  // Final queue-drain allowance.
  uint32_t timedActiveSampleMs;        // Sensor-active portion of a cycle.
  uint32_t timedCyclePeriodMs;         // Complete wake-to-wake period.
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

// Conservative fresh-traffic queue-service model. The runtime permits up to
// three sends in a slot and at most one retransmission, leaving two possible
// fresh sends. Count generated BUNDLE and STATUS frames against the node's
// once-per-frame slot and reserve at least 10% of those two fresh-send
// opportunities for bursts/control. A value of 1000 means one fresh frame per
// node slot on average.
constexpr uint32_t offeredLoadPermille(const NetworkProfile &profile) {
  return LoRaAirtime::ceilDiv(framePeriodMs(profile) * 1000u,
                             samplesPerBundle(profile) *
                                 profile.continuousSamplePeriodMs) +
         LoRaAirtime::ceilDiv(framePeriodMs(profile) * 1000u,
                             profile.statusIntervalMs);
}

constexpr uint32_t fingerprintMix(uint32_t hash,
                                  uint32_t value,
                                  uint8_t remainingBytes = 4u) {
  return remainingBytes == 0u
             ? hash
             : fingerprintMix(
                   (hash ^ static_cast<uint8_t>(value)) * 16777619u,
                   value >> 8u,
                   static_cast<uint8_t>(remainingBytes - 1u));
}

constexpr uint32_t fingerprintRadioFields(const NetworkProfile &profile,
                                          uint32_t hash) {
  return fingerprintMix(
      fingerprintMix(
          fingerprintMix(
              fingerprintMix(
                  fingerprintMix(
                      fingerprintMix(
                          fingerprintMix(
                              fingerprintMix(
                                  fingerprintMix(
                                      hash,
                                      static_cast<uint8_t>(profile.id)),
                                  profile.spreadingFactor),
                              profile.bandwidthHz),
                          profile.codingRateDenominator),
                      profile.preambleSymbols),
                  profile.explicitHeader ? 1u : 0u),
              profile.payloadCrc ? 1u : 0u),
          profile.lowDataRateOptimization ? 1u : 0u),
      static_cast<uint16_t>(profile.snrDemodFloorDbX10));
}

constexpr uint32_t fingerprintGeometryFields(const NetworkProfile &profile,
                                             uint32_t hash) {
  return fingerprintMix(
      fingerprintMix(
          fingerprintMix(
              fingerprintMix(
                  fingerprintMix(hash, profile.totalSlots),
                  profile.slotWidthMs),
              profile.guardMs),
          profile.syncStaleMs),
      profile.rxWakeAheadMs);
}

constexpr uint32_t fingerprintTrafficFields(const NetworkProfile &profile,
                                            uint32_t hash) {
  return fingerprintMix(
      fingerprintMix(
          fingerprintMix(
              fingerprintMix(
                  fingerprintMix(
                      fingerprintMix(
                          fingerprintMix(hash, profile.maxBundleDeltas),
                          profile.maxOperationalApplicationBytes),
                      profile.maxBundleAirtimeMs),
                  profile.txCompletionMarginMs),
              profile.continuousSamplePeriodMs),
          profile.timedSamplePeriodMs),
      profile.statusIntervalMs);
}

constexpr uint32_t fingerprintReliabilityFields(
    const NetworkProfile &profile,
    uint32_t hash) {
  return fingerprintMix(
      fingerprintMix(
          fingerprintMix(
              fingerprintMix(
                  fingerprintMix(
                      fingerprintMix(
                          fingerprintMix(hash,
                                         profile.useAppAckSummary ? 1u : 0u),
                          profile.reliabilityMaxAttempts),
                      profile.retryWaitMs),
                  profile.reliabilityMaxAgeMs),
              profile.awakenIntervalMs),
          profile.periodicTimeSyncMs),
      profile.commandAckTimeoutMs);
}

constexpr uint32_t fingerprintDutyFields(const NetworkProfile &profile,
                                         uint32_t hash) {
  return fingerprintMix(
      fingerprintMix(
          fingerprintMix(hash, profile.maxTxDrainBeforeStandbyMs),
          profile.timedActiveSampleMs),
      profile.timedCyclePeriodMs);
}

// Stable FNV-1a fingerprint over the fields that affect over-air
// compatibility, scheduling, offered load, or deployment interpretation.
constexpr uint32_t fingerprint(const NetworkProfile &profile) {
  return fingerprintDutyFields(
      profile,
      fingerprintReliabilityFields(
          profile,
          fingerprintTrafficFields(
              profile,
              fingerprintGeometryFields(
                  profile, fingerprintRadioFields(profile, 2166136261u)))));
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
         profile.snrDemodFloorDbX10 < 0 &&
         (profile.totalSlots == 3u || profile.totalSlots == 5u) &&
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
         offeredLoadPermille(profile) <= 1800u &&
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

// SF12 field range-test profile (select with SMARTFIRES_NETWORK_PROFILE=120).
//
// Copy/edit template -- keep the rows in this exact order:
//   { identity + modem,
//     TDMA geometry + sync,
//     BUNDLE sizing + TX timing,
//     Continuous sample, Timed sample, STATUS interval,
//     reliability + AWAKEN/TIME_SYNC/command timing,
//     final TX drain, Timed active window, Timed cycle }
//
// STATUS scheduling is independent of sensor BUNDLE sampling. The three-slot
// geometry gives each of the two nodes a transmit opportunity every 15 seconds,
// matching the STATUS heartbeat, while eight one-minute samples per BUNDLE
// keeps sensor-data traffic roughly eight minutes apart. Timed mode retains
// standard SF12 sensing/duty timing; use the Continuous node environment for
// the intended range test.
constexpr NetworkProfile kSf12RangeTest{
    // id, SF, bandwidth, CR denominator, preamble, explicit, CRC, LDO,
    // SNR floor x10
    ProfileId::Sf12, 12, 250000, 5, 8, true, true, true, -200,
    // slots, slot width, guard, stale sync, RX wake-ahead
    3, 5000, 100, 1320000, 300,
    // max deltas, max app bytes, max BUNDLE airtime, TX completion margin
    7, 111, 2216, 100,
    // Continuous sample, Timed sample, STATUS interval
    60000, 4000, 15000,
    // ACK summary, attempts, retry wait, max age, AWAKEN, TIME_SYNC,
    // command ACK
    true, 3, 50000, 150000, 27000, 125000, 180000,
    // final TX drain, Timed active window, Timed cycle
    30000, 64000, 150000};

static_assert(isValid(kSf7), "invalid SF7 network profile");
static_assert(isValid(kSf9), "invalid SF9 network profile");
static_assert(isValid(kSf10), "invalid SF10 network profile");
static_assert(isValid(kSf12), "invalid SF12 network profile");
static_assert(isValid(kSf12RangeTest), "invalid SF12 range-test profile");

constexpr const NetworkProfile &profileForSelector(uint8_t selector) {
  return selector == 7u ? kSf7
       : selector == 9u ? kSf9
       : selector == 10u ? kSf10
       : selector == 120u ? kSf12RangeTest
                          : kSf12;
}

}  // namespace NetworkProfiles

// A deployed radio build must make the selection explicitly. Native tests and
// non-network power-test firmware use SF7 only as an active-config fixture;
// neither is a deployable radio role, and all named definitions above
// remain available to native tests.
#if !defined(SMARTFIRES_NETWORK_PROFILE)
#if defined(SMARTFIRES_NATIVE_TEST) || defined(POWER_TEST)
#define SMARTFIRES_NETWORK_PROFILE 7
#else
#error "SMARTFIRES_NETWORK_PROFILE must be 7, 9, 10, 12, or 120"
#endif
#endif

#if SMARTFIRES_NETWORK_PROFILE != 7 && SMARTFIRES_NETWORK_PROFILE != 9 && \
    SMARTFIRES_NETWORK_PROFILE != 10 && SMARTFIRES_NETWORK_PROFILE != 12 && \
    SMARTFIRES_NETWORK_PROFILE != 120
#error "Unsupported SMARTFIRES_NETWORK_PROFILE; expected 7, 9, 10, 12, or 120"
#endif

namespace NetworkProfiles {
constexpr uint8_t kSelectedProfileSelector = SMARTFIRES_NETWORK_PROFILE;
static constexpr const NetworkProfile &kActiveProfile =
    profileForSelector(kSelectedProfileSelector);
constexpr uint32_t kActiveProfileFingerprint = fingerprint(kActiveProfile);
}  // namespace NetworkProfiles
