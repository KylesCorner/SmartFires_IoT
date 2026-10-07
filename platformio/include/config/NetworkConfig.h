// ---
// description: TDMA slot geometry, LoRa radio link, link-layer ACK, and app-layer reliability constants — the single source of truth for the network domain, including the node TdmaConfig profile builder.
// role: config
// docs: [bandwidth-scaling, lora-vs-lorawan, packet-reliability, software-design, tdma-protocol, tunable-parameters]
// ---
#pragma once

// Network domain — single source of truth for TDMA slot geometry, the LoRa
// radio link, link-layer ACK behavior, and app-layer (ACK-paced) reliability.
//
// This is the consolidated home for values that used to live scattered
// across TdmaConfig::tdmaCfg()'s positional-default factory, a manual
// override block in main.cpp's makeNodeTdmaCfg(), RadioHeadTdmaDriver's own
// factory defaults, and hardcoded literals inside TdmaRadioService.cpp. See
// documentation/Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md and
// documentation/Current_Architecture/TUNABLE_PARAMETERS.md for the full
// rationale and the operating-value governance process (profiles, change
// classes, rollback criteria).
//
// Rules for this header (apply to every file under include/config/):
//   - Data only: constexpr values, structs, static_asserts. No logic, no
//     Arduino/RadioHead/sensor includes.
//   - Both the node and base builds include this header, so TDMA geometry
//     (numSlots/slotWidthMs/guardMs) can never drift between them the way
//     SmartFiresBaseApp::Config::baseCfg() used to (it hardcoded its own
//     copy of these three values, disconnected from the node's build flag).

#include "config/NetworkProfiles.h"
#include "radio/TdmaConfig.h"
#include "telemetry/BinaryPacket.h"

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Build-flag resolution
// ---------------------------------------------------------------------------
// NUM_SLOTS remains as a fleet-size tripwire in PlatformIO. The selected
// profile owns the actual value, and a mismatch is rejected below.

#ifndef NUM_SLOTS
#if defined(SMARTFIRES_NATIVE_TEST)
#define NUM_SLOTS 5
#else
#error "NUM_SLOTS must match the selected network profile"
#endif
#endif

namespace NetworkConfig {

// --- TDMA geometry ----------------------------------------------------------
// Shared verbatim between node and base builds.
struct Geometry {
  uint8_t numSlots;
  uint32_t slotWidthMs;
  uint32_t guardMs;
  uint32_t syncStaleMs;
};

constexpr const NetworkProfiles::NetworkProfile &kProfile =
    NetworkProfiles::kActiveProfile;
constexpr uint8_t kProfileId = static_cast<uint8_t>(kProfile.id);
constexpr uint32_t kProfileFingerprint =
    NetworkProfiles::kActiveProfileFingerprint;

constexpr uint8_t kNumSlots = kProfile.totalSlots;
constexpr uint32_t kSlotWidthMs = kProfile.slotWidthMs;
constexpr uint32_t kGuardMs = kProfile.guardMs;
constexpr uint32_t kSyncStaleMs = kProfile.syncStaleMs;

constexpr Geometry kGeometry{kNumSlots, kSlotWidthMs, kGuardMs, kSyncStaleMs};

// One full TDMA rotation — the spacing between consecutive openings of any
// given slot, and so the natural unit for anything that has to wait for the
// base to get a turn. BaseConfig::kAckSummaryNodeSilenceMs and
// kExpectedAckIntervalMs below are both expressed in terms of it rather than
// as literals, since both go stale the moment kNumSlots changes.
constexpr uint32_t kFramePeriodMs =
    static_cast<uint32_t>(kNumSlots) * kSlotWidthMs;

// How long before slot 0 a node starts waking its radio for
// TdmaClock::baseRxWindowOpen() (see radio/TdmaConfig.h's rxWakeAheadMs for
// the full rationale: absorbs both SX1276 sleep->Rx latency and main-loop
// jitter from blocking sensor reads, neither of which guardMs accounts for).
// Starting value, not yet bench-characterized against worst-case sensor
// service time -- field-observed ACK_SUMMARY retries on the base were the
// signal that some nonzero margin is required; tune upward if retries
// persist, downward once actual wake latency is measured.
constexpr uint32_t kRxWakeAheadMs = kProfile.rxWakeAheadMs;

// --- Per-slot TX budgets -----------------------------------------------------
// Actual-length airtime plus one profile-specific completion/software margin.
// The input is the SmartFires application length; LoRaAirtime adds RadioHead's
// four-byte header before applying the selected modem tuple.
constexpr uint16_t txBudgetMs(uint16_t applicationBytes) {
  return static_cast<uint16_t>(
      NetworkProfiles::applicationAirtimeMs(kProfile, applicationBytes) +
      kProfile.txCompletionMarginMs);
}

constexpr uint16_t kBundleTxBudgetMs =
    txBudgetMs(kProfile.maxOperationalApplicationBytes);
constexpr uint16_t kAckTxWaitMs = txBudgetMs(1u);

// --- Radio link (RadioHeadTdmaDriver) ---------------------------------------
constexpr uint8_t kBaseAddr = 0x01;
constexpr uint8_t kRadioCsPin = 8;
constexpr uint8_t kRadioIntPin = 3;
constexpr uint8_t kRadioRstPin = 4;
constexpr float kRadioFrequencyMhz = 915.0f;
constexpr int8_t kRadioTxPowerDbm = 13;
constexpr uint8_t kSpreadingFactor = kProfile.spreadingFactor;
constexpr uint32_t kBandwidthHz = kProfile.bandwidthHz;
constexpr uint8_t kCodingRateDenominator = kProfile.codingRateDenominator;
constexpr uint8_t kPreambleSymbols = kProfile.preambleSymbols;
constexpr bool kExplicitHeader = kProfile.explicitHeader;
constexpr bool kPayloadCrc = kProfile.payloadCrc;
constexpr bool kLowDataRateOptimization =
    kProfile.lowDataRateOptimization;
constexpr uint8_t kMaxBundleDeltas = kProfile.maxBundleDeltas;
constexpr uint16_t kMaxOperationalApplicationBytes =
    kProfile.maxOperationalApplicationBytes;
// Preserve the field-proven SF7 behavior. Higher-SF profiles begin acceptance
// at the fixed 13 dBm baseline so dynamic power does not confound range data;
// re-enable them deliberately after profile-specific threshold validation.
constexpr bool kDynamicTxPowerDefaultEnabled = (kProfileId == 7u);

// Bounds a node clamps an incoming PKT_CMD_SET_TX_POWER to before applying it.
// The base station is the decision-maker for dynamic TX power
// (documentation/Completed_Plans/DYNAMIC_TX_POWER.md), but "the base decides" is
// not the same as "the node obeys anything" — a corrupted-but-CRC-valid frame,
// or a base running mismatched firmware, must not be able to push the radio
// somewhere the hardware cannot go.
//
// Ceiling is kRadioTxPowerDbm, deliberately *not* the SX1276's +23 dBm maximum:
// this feature only ever walks a node down from the known-working, field-
// validated baseline and back up to it. Raising the baseline itself is a manual
// change, not something a control loop gets to do.
//
// Floor is +5 dBm, the bottom of RadioHead's PA_BOOST range — begin() calls
// setTxPower(..., useRFO=false), and RH_RF95 silently clamps anything under 5
// on that path, so a lower value here would just be a lie about what the radio
// is doing.
constexpr int8_t kMinTxPowerDbm = 5;
constexpr int8_t kMaxTxPowerDbm = kRadioTxPowerDbm;
static_assert(kMinTxPowerDbm <= kMaxTxPowerDbm,
              "TX power floor must not exceed the baseline ceiling");

// Link-layer retry count / ACK timeout. Single source for two fields that
// used to be set independently and could drift: TdmaConfig::maxRetries /
// TdmaConfig::ackTimeoutMs (app-level TDMA config) and
// RadioHeadTdmaDriver::Config::retries / ::timeoutMs (the actual RadioHead
// RHReliableDatagram settings). Both now read these two constants.
constexpr uint8_t kLinkRetries = 3;
constexpr uint16_t kLinkAckTimeoutMs = 250;

// Backward-compatible maximum bound; the driver now calculates its bounded
// wait from each packet's actual length through txBudgetMs().
constexpr uint16_t kSendTxWaitMs = kBundleTxBudgetMs;

// --- TX queue / app-layer reliability (operating values shipped today) -----
constexpr uint8_t kQueueDepth = 8;
constexpr uint8_t kReliabilityWindowDepth = 8;
constexpr uint8_t kReliabilityMaxAttempts =
    kProfile.reliabilityMaxAttempts;
constexpr uint32_t kReliabilityMaxAgeMs = kProfile.reliabilityMaxAgeMs;
constexpr uint32_t kReliabilityMinRetryGapMs = 2000;
constexpr uint32_t kReliabilityFreshTrafficHoldoffMs = 2000;

// Compile-time hard caps. TdmaTxQueue::MaxDepth and
// TdmaRadioService::kMaxReliabilityWindow are defined in terms of these, so
// there is exactly one place that says "8 is the ceiling."
constexpr uint8_t kQueueCapacityHardCap = 8;
constexpr uint8_t kReliabilityWindowHardCap = 8;

constexpr TdmaReliabilityMode kReliabilityMode =
    kProfile.useAppAckSummary
        ? TdmaReliabilityMode::AppLayerAckSummary
        : TdmaReliabilityMode::StrictLinkAck;

// --- ACK-paced retry gate (APP_ACK_SUMMARY mode) ----------------------------
// See documentation/Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md
// Appendix B for the retry-wait derivation, implementation status, and the
// open decision on whether expectedAckIntervalMs should instead be derived
// from the base's ackSummaryMinIntervalMs (BaseConfig.h).
// The base can only transmit in slot 0, so no matter how dirty a node's
// tracker is, its ACK_SUMMARY cannot arrive more often than once per frame
// rotation. Deriving this from kFramePeriodMs rather than leaving it the
// former literal 4000 keeps the node's retry-wait gate honest as the network
// grows: at NUM_SLOTS=4 the two happened to be close (3600 vs 4000), but at
// NUM_SLOTS=5 a literal 4000 would have the node assuming acks arrive faster
// than the base can physically send them, shrinking the retry margin from
// 2.2 ack intervals to 1.8 and making a single lost ack much likelier to
// provoke a spurious retransmit.
constexpr uint32_t kExpectedAckIntervalMs = kFramePeriodMs;
constexpr uint16_t kRetryWaitMultiplierPermille = 2000;  // 2.0x
constexpr uint32_t kRetryWaitMinMs = kProfile.retryWaitMs;
constexpr uint32_t kRetryWaitMaxMs = kProfile.retryWaitMs;
constexpr bool kRequireAckSummaryBeforeFirstRetry = false;

// --- Node packet cadence ------------------------------------------------------
constexpr uint32_t kStatusIntervalMs = kProfile.statusIntervalMs;

// --- Boot handshake ----------------------------------------------------------
// SmartFiresNodeApp re-broadcasts PKT_AWAKEN at this interval until the
// first TIME_SYNC is received; sensors are withheld until then.
constexpr uint32_t kAwakenIntervalMs = kProfile.awakenIntervalMs;

// --- Telemetry TX gate -------------------------------------------------------
// Set false to suppress bundle encoding and transmission (STATUS still flows).
// Flip to true for normal operation.
constexpr bool kEnableTelemetryTx = true;

// ---------------------------------------------------------------------------
// Compile-time invariants
// ---------------------------------------------------------------------------
// One StrictLinkAck transaction must fit in a slot: one TX burst, one ACK wait,
// and guard bands. TdmaRadioService checks the remaining budget before each
// send; AppLayerAckSummary does not use this remote ACK wait and may fit two
// maximum bundles in the usable 860 ms span. This assertion conservatively
// covers the alternative StrictLinkAck mode.
// 340 (bundle TX) + 250 (ACK timeout) + 2×20 (guard) = 630 ms < 900 ms ✓
static_assert(NUM_SLOTS == kNumSlots,
              "NUM_SLOTS must match the selected network profile");
static_assert(kMaxBundleDeltas <= BinaryPacket::kBundleMaxDeltas,
              "operational bundle cap exceeds the wire-format ceiling");
static_assert(NetworkProfiles::kBundleFixedBytes ==
                  sizeof(BinaryPacket::PktHeader) +
                      sizeof(BinaryPacket::FullStatePayload) + 2u,
              "profile bundle-size model drifted from the wire format");
static_assert(NetworkProfiles::kBundleBytesPerDelta ==
                  sizeof(BinaryPacket::DeltaPayload),
              "profile delta-size model drifted from the wire format");
static_assert(kMaxOperationalApplicationBytes <=
                  BinaryPacket::kMaxBundleLoRaSize,
              "operational bundle size exceeds the wire-format ceiling");
static_assert(kReliabilityMode != TdmaReliabilityMode::StrictLinkAck ||
                  kSlotWidthMs > kBundleTxBudgetMs + kLinkAckTimeoutMs +
                                     2 * kGuardMs,
              "slot too small for strict-link-ACK bundle transaction");
static_assert(kRetryWaitMinMs <= kRetryWaitMaxMs,
              "retryWaitMinMs must not exceed retryWaitMaxMs");
// The retry-wait floor has to cover at least one full frame rotation, or a
// node can give up on an ack before the base has had a single opportunity to
// send it — every retry would then be spent racing a window that was never
// open. Adding nodes grows kFramePeriodMs, so this is the assertion that
// fires when the slot count outgrows the floor (at 900 ms slots, NUM_SLOTS=6
// is the first count that trips it) and forces kRetryWaitMinMs/kRetryWaitMaxMs
// to be re-derived rather than silently under-waiting.
static_assert(kRetryWaitMinMs >= kFramePeriodMs,
              "retryWaitMinMs must cover at least one TDMA frame period");
static_assert(kQueueDepth <= kQueueCapacityHardCap,
              "queueDepth exceeds TdmaTxQueue's compile-time capacity");
static_assert(kReliabilityWindowDepth <= kReliabilityWindowHardCap,
              "reliabilityWindowDepth exceeds TdmaRadioService's compile-time capacity");
static_assert(TdmaConfig::MaxPayloadLen >= BinaryPacket::kMaxBundleLoRaSize,
              "MaxPayloadLen must stay >= BinaryPacket::kMaxBundleLoRaSize");

// ---------------------------------------------------------------------------
// Profile builder
// ---------------------------------------------------------------------------

// Fully-populated TdmaConfig for every node build. nodeId is always unassigned
// (0) at construction; the base assigns the real value from uid_hash once
// TIME_SYNC is exchanged.
inline TdmaConfig nodeTdmaProfile() {
  TdmaConfig cfg;
  cfg.nodeId = 0;
  cfg.baseAddr = kBaseAddr;
  cfg.numSlots = kNumSlots;
  cfg.slotWidthMs = kSlotWidthMs;
  cfg.guardMs = kGuardMs;
  cfg.syncStaleMs = kSyncStaleMs;
  cfg.rxWakeAheadMs = kRxWakeAheadMs;
  cfg.queueDepth = kQueueDepth;
  cfg.enableLinkAck = (kReliabilityMode == TdmaReliabilityMode::StrictLinkAck);
  cfg.maxRetries = kLinkRetries;
  cfg.ackTimeoutMs = kLinkAckTimeoutMs;
  cfg.enableAppReliability = true;
  cfg.reliabilityWindowDepth = kReliabilityWindowDepth;
  cfg.reliabilityMaxAttempts = kReliabilityMaxAttempts;
  cfg.reliabilityMaxAgeMs = kReliabilityMaxAgeMs;
  cfg.reliabilityMinRetryGapMs = kReliabilityMinRetryGapMs;
  cfg.reliabilityFreshTrafficHoldoffMs = kReliabilityFreshTrafficHoldoffMs;
  cfg.reliabilityMode = kReliabilityMode;
  cfg.expectedAckIntervalMs = kExpectedAckIntervalMs;
  cfg.retryWaitMultiplierPermille = kRetryWaitMultiplierPermille;
  cfg.retryWaitMinMs = kRetryWaitMinMs;
  cfg.retryWaitMaxMs = kRetryWaitMaxMs;
  cfg.requireAckSummaryBeforeFirstRetry = kRequireAckSummaryBeforeFirstRetry;
  return cfg;
}

}  // namespace NetworkConfig
