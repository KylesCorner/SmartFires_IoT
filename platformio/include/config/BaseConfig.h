// ---
// description: Base-station bridge constants (USB-serial cadence, ACK-summary batching, periodic TIME_SYNC, node/ACK table sizes) — reuses NetworkConfig::kGeometry for TDMA slot geometry.
// role: config
// docs: [packet-reliability, tunable-parameters]
// ---
#pragma once

// Base-bridge domain — single source of truth for the base station's
// USB-serial-to-Jetson bridge behavior, ACK-summary batching cadence, the
// periodic fallback TIME_SYNC broadcast, and node/ACK-tracking table sizes.
//
// Reuses NetworkConfig::kGeometry for TDMA slot geometry instead of
// hardcoding a second, independent copy — SmartFiresBaseApp::Config::
// baseCfg() used to default tdmaNumSlots/tdmaSlotWidthMs/tdmaGuardMs on its
// own (4/900/20), completely disconnected from the node's NUM_SLOTS build
// flag. `platformio.ini` now supplies one shared flag to the base and every
// node, and this file consumes the resulting NetworkConfig geometry.
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

// Fallback for a Timed node whose PKT_WINDOW_END frame was itself lost, so the
// base never learned it was entering standby. Silence longer than this
// gates ACK_SUMMARY the same way the explicit marker does — the tracker keeps
// `dirty`, so the ack is still deferred rather than dropped.
//
// Two frame periods. One frame is the natural spacing between the base's own
// slot-0 windows, so this permits a first attempt (the ack may simply have been
// lost, which is worth one retry) and gates the rest, instead of spending three
// ~1 s blocking sendToWait() calls on a node that cannot answer. A node with
// nothing new to say never has `dirty` set, so this can only ever gate a node
// that really did stop responding.
constexpr uint32_t kAckSummaryNodeSilenceMs = 2u * NetworkConfig::kFramePeriodMs;

// Bounded retry for a queued command when RadioHead refuses the local send()
// request. Command delivery is otherwise fire-and-forget and confirmed later
// by PKT_CMD_ACK; a missing remote response does not keep this queue entry.
// Without this cap, an unhealthy local radio could retain one entry forever.
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
// documentation/Completed_Plans/TUNABLE_PARAMETER_ARCHITECTURE_PLAN.md
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

// --- Dynamic TX power control loop (TxPowerController) ----------------------
// See documentation/Completed_Plans/DYNAMIC_TX_POWER.md. The base is the only
// decision-maker; the node applies absolute levels and reports what it applied.
// None of these are bench-characterized yet — they are starting values chosen
// to be conservative, and the loop's own debug log is the instrument for tuning
// them.

// How many nodes the controller tracks. Matches kMaxAckTrackedNodes so the two
// per-node tables can never disagree about how many nodes the base can follow.
constexpr uint8_t kMaxTxPowerTrackedNodes = kMaxAckTrackedNodes;

// SNR, in tenths of a dB, at which the modem stops being able to demodulate.
// -7.5 dB is the SX1276's figure for SF7, which is what every node runs today
// (RadioHead's implicit Bw125Cr45Sf128 default — see LORA_VS_LORAWAN.md). This
// is the reference point link margin is measured against; it must change if the
// spreading factor ever becomes configurable, since the floor is SF-dependent.
constexpr int16_t kSnrDemodFloorDbX10 = -75;

// Link margin the loop aims to leave above the demod floor, in tenths of a dB.
// A single target rather than the separate floor/headroom threshold pair the
// plan first proposed: two independent thresholds can be set into an
// oscillation (floor above headroom) with nothing to catch it, whereas one
// target plus a dead band cannot.
constexpr int16_t kTargetSnrMarginDbX10 = 100;  // 10.0 dB

// Step size for a power *reduction*, in dB. Power increases are not stepped —
// see TxPowerController::decide() for why recovery is a single jump.
constexpr int8_t kTxPowerStepDbm = 2;

// Extra margin above the target required before stepping down, in tenths of a
// dB. Without it a node sitting exactly at the target oscillates: step down,
// fall below target, step back up, forever. The static_assert below is the
// actual guarantee — the dead band must exceed one step, or a step-down can
// land the node under target and immediately re-trigger a step-up.
constexpr int16_t kSnrDeadBandDbX10 = 30;  // 3.0 dB
static_assert(kSnrDeadBandDbX10 > static_cast<int16_t>(kTxPowerStepDbm) * 10,
              "TX power dead band must exceed one step, or the loop oscillates");

// Minimum time between two decisions for the same node.
//
// This — not STATUS arrival — is what paces the loop, and it is the reason the
// plan's "what do we do about the debug env" question does not need answering.
// STATUS interval is a build flag. All current node environments select 15 s,
// while PacketHandler's unconfigured fallback remains 15 min. Pacing this loop
// with its own clock keeps threshold behavior stable if a future build chooses
// a different STATUS cadence: STATUS only triggers consideration, and retx/fail
// deltas are measured across this fixed window.
constexpr uint32_t kTxPowerMinDecisionIntervalMs = 60000;  // 60 s

// How long to wait for the CMD_ACK confirming a power change before giving up
// and re-arming the decision.
//
// Must exceed the Timed duty-cycle period (SensingConfig::kTimedCyclePeriodMs,
// 75 s): a command queued while the node is in MCU standby physically cannot be
// delivered until its next PKT_WINDOW_BEGIN. Sized off a frame period instead —
// the obvious-looking choice — the base would time out and re-arm on every node
// that was merely asleep.
constexpr uint32_t kCmdAckTimeoutMs = 120000;  // 120 s

// Silence after which a node's TX power is treated as unknown and it is probed
// back to baseline. This is the uplink-dead case: the base has stopped hearing
// the node but may still be able to reach it, and it is the only actor that
// can, since a node with a working downlink never trips its own stale-sync
// fallback. Comfortably longer than a full duty cycle so a sleeping node is
// never mistaken for a dark one.
constexpr uint32_t kTxPowerSilenceTimeoutMs = 300000;  // 5 min
static_assert(kTxPowerSilenceTimeoutMs > kCmdAckTimeoutMs,
              "silence timeout must outlast a pending command's ack window");

// Bound on best-effort baseline probes sent into silence before giving up on a
// node. Unbounded probing would spend the base's slot-0 airtime forever on a
// node that is simply gone. Reset when the node is heard from again.
constexpr uint8_t kMaxTxPowerSilenceProbes = 3;

}  // namespace BaseConfig
