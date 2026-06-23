// ---
// description: TDMA slot geometry, LoRa radio link, link-layer ACK, and app-layer reliability constants — the single source of truth for the network domain, including the node TdmaConfig profile builder.
// role: config
// docs: [bandwidth-scaling, packet-reliability, tdma-protocol, tunable-parameters]
// ---
#pragma once

// Network domain — single source of truth for TDMA slot geometry, the LoRa
// radio link, link-layer ACK behavior, and app-layer (ACK-paced) reliability.
//
// This is the consolidated home for values that used to live scattered
// across TdmaConfig::tdmaCfg()'s positional-default factory, a manual
// override block in main.cpp's makeNodeTdmaCfg(), RadioHeadTdmaDriver's own
// factory defaults, and hardcoded literals inside TdmaRadioService.cpp. See
// documentation/Pending_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md and
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

#include "radio/TdmaConfig.h"
#include "telemetry/BinaryPacket.h"

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Build-flag resolution
// ---------------------------------------------------------------------------
// NUM_SLOTS and SMARTFIRES_TDMA_RELIABILITY_MODE are set per-environment in
// platformio.ini (node/node_debug envs). This is the one place that reads
// the raw -D flags and turns them into typed values — main.cpp no longer
// repeats these #ifndef guards itself.
//
// SMARTFIRES_STATUS_INTERVAL_MS lives here rather than in SensingConfig.h
// because it directly drives offered radio load (see Appendix A, table A,
// and Compatibility Rule 5), which makes it a Network-domain concern even
// though it's read by PacketHandler.

#ifndef NUM_SLOTS
#define NUM_SLOTS 4
#endif

#ifndef SMARTFIRES_TDMA_RELIABILITY_MODE
#define SMARTFIRES_TDMA_RELIABILITY_MODE 0
#endif

#ifndef SMARTFIRES_STATUS_INTERVAL_MS
#define SMARTFIRES_STATUS_INTERVAL_MS (15UL * 60UL * 1000UL)
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

constexpr uint8_t kNumSlots = NUM_SLOTS;
constexpr uint32_t kSlotWidthMs = 900;
constexpr uint32_t kGuardMs = 20;
constexpr uint32_t kSyncStaleMs = 1320000;  // 22 min

constexpr Geometry kGeometry{kNumSlots, kSlotWidthMs, kGuardMs, kSyncStaleMs};

// --- Per-slot TX budgets -----------------------------------------------------
// Conservative slot-budget estimates used by TdmaRadioService::drainTxQueue()
// to avoid crossing into the next node's slot. Named here (rather than left
// as magic numbers inside TdmaRadioService.cpp's estimateTxBudgetMs()) so the
// slot-width invariant below is checkable at compile time.
constexpr uint16_t kBundleTxBudgetMs = 340;
constexpr uint16_t kStatusTxBudgetMs = 120;
constexpr uint16_t kAwakenTxBudgetMs = 90;
constexpr uint16_t kDefaultTxBudgetMs = 140;  // unknown / FULL_STATE payloads

// --- Radio link (RadioHeadTdmaDriver) ---------------------------------------
constexpr uint8_t kBaseAddr = 0x01;
constexpr uint8_t kRadioCsPin = 8;
constexpr uint8_t kRadioIntPin = 3;
constexpr uint8_t kRadioRstPin = 4;
constexpr float kRadioFrequencyMhz = 915.0f;
constexpr int8_t kRadioTxPowerDbm = 13;
constexpr uint16_t kRadioCadTimeoutMs = 10;

// Link-layer retry count / ACK timeout. Single source for two fields that
// used to be set independently and could drift: TdmaConfig::maxRetries /
// TdmaConfig::ackTimeoutMs (app-level TDMA config) and
// RadioHeadTdmaDriver::Config::retries / ::timeoutMs (the actual RadioHead
// RHReliableDatagram settings). Both now read these two constants.
constexpr uint8_t kLinkRetries = 3;
constexpr uint16_t kLinkAckTimeoutMs = 250;

// --- TX queue / app-layer reliability (operating values shipped today) -----
constexpr uint8_t kQueueDepth = 8;
constexpr uint8_t kReliabilityWindowDepth = 8;
constexpr uint8_t kReliabilityMaxAttempts = 3;
constexpr uint32_t kReliabilityMaxAgeMs = 30000;
constexpr uint32_t kReliabilityMinRetryGapMs = 2000;
constexpr uint32_t kReliabilityFreshTrafficHoldoffMs = 2000;

// Compile-time hard caps. TdmaTxQueue::MaxDepth and
// TdmaRadioService::kMaxReliabilityWindow are defined in terms of these, so
// there is exactly one place that says "8 is the ceiling."
constexpr uint8_t kQueueCapacityHardCap = 8;
constexpr uint8_t kReliabilityWindowHardCap = 8;

constexpr TdmaReliabilityMode kReliabilityMode =
    tdmaReliabilityModeFromValue(SMARTFIRES_TDMA_RELIABILITY_MODE);

// --- ACK-paced retry gate (APP_ACK_SUMMARY mode) ----------------------------
// See documentation/Pending_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md
// Appendix B for the retry-wait derivation, implementation status, and the
// open decision on whether expectedAckIntervalMs should instead be derived
// from the base's ackSummaryMinIntervalMs (BaseConfig.h).
constexpr uint32_t kExpectedAckIntervalMs = 4000;
constexpr uint16_t kRetryWaitMultiplierPermille = 2000;  // 2.0x
constexpr uint32_t kRetryWaitMinMs = 4500;
constexpr uint32_t kRetryWaitMaxMs = 10000;
constexpr bool kRequireAckSummaryBeforeFirstRetry = false;

// --- Node packet cadence ------------------------------------------------------
constexpr uint32_t kStatusIntervalMs = SMARTFIRES_STATUS_INTERVAL_MS;

// --- Boot handshake ----------------------------------------------------------
// SmartFiresNodeApp re-broadcasts PKT_AWAKEN at this interval until the
// first TIME_SYNC is received; sensors are withheld until then.
constexpr uint32_t kAwakenIntervalMs = 5000;

// --- Telemetry TX gate -------------------------------------------------------
// Set false to suppress bundle encoding and transmission (STATUS still flows).
// Flip to true for normal operation.
constexpr bool kEnableTelemetryTx = true;

// ---------------------------------------------------------------------------
// Compile-time invariants
// ---------------------------------------------------------------------------
// Worst-case per-slot occupancy: ONE TX burst + ONE ACK timeout + guard bands.
// (TdmaRadioService checks budget before each send, so at most one bundle can
// start in a slot regardless of kLinkRetries.  In APP_ACK_SUMMARY mode
// enableLinkAck=false, so kLinkAckTimeoutMs doesn't apply at all — this
// assertion is a conservative upper bound that also covers StrictLinkAck mode.)
// 340 (bundle TX) + 250 (ACK timeout) + 2×20 (guard) = 630 ms < 900 ms ✓
static_assert(kSlotWidthMs > kBundleTxBudgetMs + kLinkAckTimeoutMs + 2 * kGuardMs,
              "slotWidthMs too small for worst-case bundle TX + ACK + guard");
static_assert(kRetryWaitMinMs <= kRetryWaitMaxMs,
              "retryWaitMinMs must not exceed retryWaitMaxMs");
static_assert(kQueueDepth <= kQueueCapacityHardCap,
              "queueDepth exceeds TdmaTxQueue's compile-time capacity");
static_assert(kReliabilityWindowDepth <= kReliabilityWindowHardCap,
              "reliabilityWindowDepth exceeds TdmaRadioService's compile-time capacity");
static_assert(TdmaConfig::MaxPayloadLen >= BinaryPacket::kMaxBundleLoRaSize,
              "MaxPayloadLen must stay >= BinaryPacket::kMaxBundleLoRaSize");

// ---------------------------------------------------------------------------
// Profile builder
// ---------------------------------------------------------------------------

// Fully-populated TdmaConfig for every node build (node, node_debug — they
// differ only in SMARTFIRES_STATUS_INTERVAL_MS, which PacketHandler reads
// separately). nodeId is always unassigned (0) at construction; the base
// assigns the real value from uid_hash once TIME_SYNC is exchanged.
inline TdmaConfig nodeTdmaProfile() {
  TdmaConfig cfg;
  cfg.nodeId = 0;
  cfg.baseAddr = kBaseAddr;
  cfg.numSlots = kNumSlots;
  cfg.slotWidthMs = kSlotWidthMs;
  cfg.guardMs = kGuardMs;
  cfg.syncStaleMs = kSyncStaleMs;
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
