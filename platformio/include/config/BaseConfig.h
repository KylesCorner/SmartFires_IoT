// ---
// description: Base-station bridge constants (UART-to-Jetson cadence, ACK-summary batching, periodic TIME_SYNC, node/ACK table sizes) — reuses NetworkConfig::kGeometry for TDMA slot geometry.
// role: config
// docs: [tunable-parameters]
// ---
#pragma once

// Base-bridge domain — single source of truth for the base station's
// UART-to-Jetson bridge behavior, ACK-summary batching cadence, the
// periodic fallback TIME_SYNC broadcast, and node/ACK-tracking table sizes.
//
// Reuses NetworkConfig::kGeometry for TDMA slot geometry instead of
// hardcoding a second, independent copy — SmartFiresBaseApp::Config::
// baseCfg() used to default tdmaNumSlots/tdmaSlotWidthMs/tdmaGuardMs on its
// own (4/900/20), completely disconnected from the node's NUM_SLOTS build
// flag, and the base build doesn't even receive -DNUM_SLOTS. This file is
// the fix for that drift.
//
// Data only — no logic, no Arduino includes.

#include "config/NetworkConfig.h"

#include <stdint.h>

namespace BaseConfig {

constexpr uint8_t kBaseAddr = NetworkConfig::kBaseAddr;
constexpr uint8_t kTimeSyncBroadcastAddr = 0xFF;
constexpr uint32_t kUartBaud = 115200;
constexpr uint32_t kAckSummaryMinIntervalMs = 25;

// TDMA geometry: shared with the node builds via NetworkConfig::kGeometry,
// so these three can no longer drift from NUM_SLOTS/slotWidthMs/guardMs.
constexpr uint8_t kTdmaNumSlots = NetworkConfig::kGeometry.numSlots;
constexpr uint32_t kTdmaSlotWidthMs = NetworkConfig::kGeometry.slotWidthMs;
constexpr uint32_t kTdmaGuardMs = NetworkConfig::kGeometry.guardMs;

// Periodic fallback TIME_SYNC broadcast, sent unconditionally by the base
// firmware regardless of what the Jetson is doing. This is intentionally a
// *different*, much shorter cadence than the Jetson's --sync-interval
// (default 600 s, see smartfires_edge/config.py): if the Jetson ingest
// process is down or hasn't sent a sync recently, nodes still get a
// base-originated sync at this cadence instead of drifting all the way to
// syncStaleMs (22 min) and falling back to unconditional TX. The two are
// not meant to be equal. See
// documentation/Pending_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md
// Appendix A, Open Decision 3, for whether this should remain a documented
// failsafe or be removed once Jetson-driven sync is proven reliable.
constexpr uint32_t kPeriodicTimeSyncMs = 50000;

constexpr uint32_t kHealthLogPeriodMs = 5000;

// --- Node / ACK-tracking table sizes ----------------------------------------
// kTotalEntities is the network's total slot count *including the base* —
// it must equal NetworkConfig::kNumSlots (one slot per node) plus implicit
// base participation, since each node assignment consumes one TDMA slot.
// This used to be an independently hardcoded `4`, which happened to match
// NUM_SLOTS=4 by coincidence rather than by construction — exactly the kind
// of drift this consolidation closes.
constexpr uint8_t kTotalEntities = NetworkConfig::kNumSlots;
constexpr uint8_t kMaxAssignedNodes = kTotalEntities - 1;
constexpr uint8_t kFirstNodeId = 0x02;
constexpr uint8_t kMaxAckTrackedNodes = 16;

}  // namespace BaseConfig
