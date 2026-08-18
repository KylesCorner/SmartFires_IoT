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

// Bounded retry for ACK_SUMMARY before giving up on an unreachable node.
// Each attempt is one sendAckSummary() call, which already contains
// RHReliableDatagram's own link-layer retry burst (kLinkRetries @
// kLinkAckTimeoutMs) — this counts app-level attempts on top of that, not
// individual radio transmissions. 3 = 1 normal attempt + 2 extra. After this
// many consecutive failures, the tracker is held (no further attempts) until
// new telemetry arrives from that node or it re-AWAKENs.
constexpr uint8_t kMaxAckSummarySendAttempts = 3;

// Fallback for a Timed node whose PKT_FLAG_WINDOW_LAST frame was itself lost,
// so the base never learned it was entering standby. Silence longer than this
// gates ACK_SUMMARY the same way the explicit marker does — the tracker keeps
// `dirty`, so the ack is still deferred rather than dropped.
//
// Two frame periods. One frame is the natural spacing between the base's own
// slot-0 windows, so this permits a first attempt (the ack may simply have been
// lost, which is worth one retry) and gates the rest, instead of spending three
// ~1 s blocking sendToWait() calls on a node that cannot answer. A node with
// nothing new to say never has `dirty` set, so this can only ever gate a node
// that really did stop responding.
constexpr uint32_t kAckSummaryNodeSilenceMs =
    2u * static_cast<uint32_t>(NetworkConfig::kNumSlots) *
    NetworkConfig::kSlotWidthMs;

// Bounded retry for a queued CMD_CALIBRATE/CMD_RESET before giving up on a
// node that isn't link-acking it. Each attempt is one sendToWait() call from
// sendPendingCommand(), which already contains RHReliableDatagram's own
// link-layer retry burst (kLinkRetries @ kLinkAckTimeoutMs) — this counts
// base-window attempts on top of that (one per ~frame period), not
// individual radio transmissions. Without this cap, a node that never
// link-acks (e.g. it already rebooted and missed the window) would have its
// queued command retried forever, once per base window, permanently
// occupying one of the kMaxPendingCommands slots.
constexpr uint8_t kMaxPendingCommandSendAttempts = 3;

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
