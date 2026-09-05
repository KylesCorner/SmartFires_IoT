// ---
// description: Base-station decision loop for per-node dynamic LoRa TX power — accumulates link margin, decides step-down/jump-up, and tracks pending/silent nodes.
// role: implementation
// ---
#pragma once

#include "config/BaseConfig.h"
#include "config/NetworkConfig.h"
#include "telemetry/BinaryPacket.h"

#include <stdint.h>

// Owns the base station's per-node TX power decisions.
//
// Deliberately pure logic: no radio, no clock, no logging. Every input arrives
// as an explicit call and every output is a returned Decision the caller is
// free to act on or drop. That makes the whole control loop testable on native
// without a fake radio, and it keeps the "who decides" boundary literal — this
// class decides, SmartFiresBaseApp transmits.
//
// The controlling ideas (documentation/Completed_Plans/DYNAMIC_TX_POWER.md):
//
//   * Every failure mode is owned by whichever side still has a working link.
//     This class owns the two the *base* can act on — a linked node whose
//     margin is wrong, and a node that has gone silent while the downlink still
//     works. The node owns the third (downlink dead, so it can no longer be
//     commanded at all) via its own stale-sync revert; nothing here duplicates
//     that.
//   * Commands are absolute, never relative, so an unknown or desynced node is
//     always recoverable by commanding baseline. This class therefore never
//     needs to be *right* about a node's current level to be *safe*.
//   * Down slowly, up in one jump. Being a step too high costs a sliver of
//     battery; being a step too low costs telemetry and the retries to recover
//     it, which burn more energy than the step ever saved.
class TxPowerController {
public:
  enum class Action : uint8_t {
    None,
    // Send CMD_SET_TX_POWER with Decision::targetDbm.
    SetPower,
  };

  enum class Reason : uint8_t {
    None,
    MarginLow,        // link margin under target — jump to baseline
    HeadroomStepDown, // margin comfortably over target — step down one
    SilenceProbe,     // node has gone quiet; blind probe back to baseline
  };

  struct Decision {
    Action action = Action::None;
    uint8_t nodeId = 0;
    int8_t targetDbm = 0;
    Reason reason = Reason::None;
    // Margin (tenths of a dB above the demod floor) the decision was made on.
    // Carried purely so the caller can log *why* without recomputing it.
    int16_t marginDbX10 = 0;
  };

  // Every inbound frame from a node, of any type. Feeds the margin average and
  // the silence timer. Cheap enough to call on the RX hot path.
  void onPacketReceived(uint8_t nodeId, int8_t snrDb, uint32_t nowMs);

  // A STATUS packet's contents. This is the trigger to *consider* a decision;
  // whether one is actually made is paced by kTxPowerMinDecisionIntervalMs, not
  // by how often STATUS arrives.
  //
  // txPowerDbm/isStatic are the node's own reported ground truth and overwrite
  // what this controller believed — a node that rebooted, was clamped, or was
  // set static out-of-band is authoritative about itself.
  Decision onStatus(uint8_t nodeId, uint16_t retxTotal, uint16_t failTotal,
                    int8_t txPowerDbm, bool isStatic, uint32_t nowMs);

  // CMD_ACK for a previously-sent power change. Clears the in-flight gate.
  void onCmdAck(uint8_t nodeId, uint32_t nowMs);

  // Node re-announced itself, so it rebooted and is back at the static
  // baseline in DYNAMIC mode. Drops every accumulated statistic: retx/fail
  // counters restart at zero on the node, so differencing across a reboot
  // would produce a garbage (usually hugely negative) delta.
  void onAwaken(uint8_t nodeId, uint32_t nowMs);

  // Operator override relayed through the base. Recorded even if the command
  // never reaches the node: a node wrongly believed STATIC is merely left
  // alone, which is safe, whereas one wrongly believed DYNAMIC would be
  // stepped against the operator's wishes.
  void setMode(uint8_t nodeId, uint8_t mode);

  // Called every tick. Handles the two time-driven transitions the packet path
  // cannot: expiring an unacked command, and probing a node that has gone
  // silent. Returns at most one decision per call, so a caller with a single
  // command queue slot per window is never handed more than it can send.
  Decision update(uint32_t nowMs);

  // Caller reports back what it did with a Decision. A command that failed to
  // enqueue must NOT arm the in-flight gate — the decision simply re-arms and
  // is retried next interval, which is the documented behaviour for a full
  // command queue.
  void onCommandSent(uint8_t nodeId, int8_t targetDbm, uint32_t nowMs);
  void onCommandFailed(uint8_t nodeId);

  // Observability accessors — used by the base's health log.
  bool known(uint8_t nodeId) const;
  int8_t currentDbm(uint8_t nodeId) const;
  bool isStatic(uint8_t nodeId) const;

  static const char *reasonName(Reason reason);

private:
  struct Node {
    bool inUse = false;
    uint8_t nodeId = 0;
    uint8_t mode = BinaryPacket::TX_POWER_MODE_DYNAMIC;

    // Last level believed applied. Seeded from the static baseline and
    // corrected by every STATUS, which is ground truth.
    int8_t currentDbm = NetworkConfig::kRadioTxPowerDbm;

    bool hasPending = false;
    int8_t pendingDbm = 0;
    uint32_t pendingSinceMs = 0;

    // Margin accumulator. A running sum rather than an EMA: it is reset at
    // every decision, so it always describes exactly the interval the decision
    // covers, and it is auditable in a way an EMA's implicit history is not.
    int32_t snrSumDb = 0;
    uint16_t snrCount = 0;

    bool haveLinkStats = false;
    uint16_t lastRetxTotal = 0;
    uint16_t lastFailTotal = 0;

    bool haveDecided = false;
    uint32_t lastDecisionMs = 0;

    bool heard = false;
    uint32_t lastHeardMs = 0;
    uint8_t silenceProbes = 0;
  };

  Node *find(uint8_t nodeId);
  const Node *find(uint8_t nodeId) const;
  Node *findOrCreate(uint8_t nodeId);

  // The decision rule itself, factored out so it is readable in one screen.
  // Owns closing the decision window (margin samples, link-stat baseline, rate
  // limit clock) so no caller can advance one without the others.
  Decision decide(Node &node, uint16_t retxTotal, uint16_t failTotal,
                  uint32_t nowMs);

  static int8_t clampDbm(int32_t dbm);

  Node _nodes[BaseConfig::kMaxTxPowerTrackedNodes] = {};
};
