// ---
// description: Implements TxPowerController's margin accumulation, step-down/jump-up decision rule, pending-command expiry, and silence probing.
// role: implementation
// ---
#include "radio/TxPowerController.h"

TxPowerController::Node *TxPowerController::find(uint8_t nodeId) {
  for (auto &n : _nodes) {
    if (n.inUse && n.nodeId == nodeId) {
      return &n;
    }
  }
  return nullptr;
}

const TxPowerController::Node *TxPowerController::find(uint8_t nodeId) const {
  for (const auto &n : _nodes) {
    if (n.inUse && n.nodeId == nodeId) {
      return &n;
    }
  }
  return nullptr;
}

TxPowerController::Node *TxPowerController::findOrCreate(uint8_t nodeId) {
  if (Node *existing = find(nodeId)) {
    return existing;
  }
  for (auto &n : _nodes) {
    if (!n.inUse) {
      n = Node{};
      n.inUse = true;
      n.nodeId = nodeId;
      return &n;
    }
  }
  // Table full. Returning null rather than evicting: every entry belongs to a
  // node the base is actively tracking, and silently dropping one would leave
  // that node's power unmanaged with nothing to say so.
  return nullptr;
}

int8_t TxPowerController::clampDbm(int32_t dbm) {
  if (dbm < NetworkConfig::kMinTxPowerDbm) {
    return NetworkConfig::kMinTxPowerDbm;
  }
  if (dbm > NetworkConfig::kMaxTxPowerDbm) {
    return NetworkConfig::kMaxTxPowerDbm;
  }
  return static_cast<int8_t>(dbm);
}

void TxPowerController::onPacketReceived(uint8_t nodeId, int8_t snrDb,
                                         uint32_t nowMs) {
  Node *node = findOrCreate(nodeId);
  if (node == nullptr) {
    return;
  }

  node->snrSumDb += snrDb;
  if (node->snrCount < 0xFFFFu) {
    node->snrCount++;
  }

  node->heard = true;
  node->lastHeardMs = nowMs;
  // Hearing anything at all means the node is not dark, so the probe budget is
  // spent only on genuinely consecutive silences.
  node->silenceProbes = 0;
}

TxPowerController::Decision TxPowerController::onStatus(uint8_t nodeId,
                                                       uint16_t retxTotal,
                                                       uint16_t failTotal,
                                                       int8_t txPowerDbm,
                                                       bool isStaticMode,
                                                       uint32_t nowMs) {
  Decision none;
  Node *node = findOrCreate(nodeId);
  if (node == nullptr) {
    return none;
  }

  // The node's own report wins over what this controller believed. It is the
  // only party that knows what its radio is actually set to — the value here
  // may be stale across a reboot, a clamp, or a lost CMD_ACK.
  node->currentDbm = txPowerDbm;
  node->mode = isStaticMode ? BinaryPacket::TX_POWER_MODE_STATIC
                            : BinaryPacket::TX_POWER_MODE_DYNAMIC;

  // First STATUS since a reset establishes the baseline; there is no delta to
  // compute against yet.
  if (!node->haveLinkStats) {
    node->lastRetxTotal = retxTotal;
    node->lastFailTotal = failTotal;
    node->haveLinkStats = true;
  }

  return decide(*node, retxTotal, failTotal, nowMs);
}

TxPowerController::Decision TxPowerController::decide(Node &node,
                                                      uint16_t retxTotal,
                                                      uint16_t failTotal,
                                                      uint32_t nowMs) {
  Decision decision;
  decision.nodeId = node.nodeId;

  // An operator has pinned this node. The loop's entire job is to not touch it.
  if (node.mode == BinaryPacket::TX_POWER_MODE_STATIC) {
    return decision;
  }

  // One change in flight at a time. Compounding an unconfirmed change with
  // another means never learning which one the node actually applied.
  if (node.hasPending) {
    return decision;
  }

  // Rate limit. This is what makes the loop's behaviour independent of the
  // STATUS interval build flag — see BaseConfig::kTxPowerMinDecisionIntervalMs.
  if (node.haveDecided &&
      (nowMs - node.lastDecisionMs) < BaseConfig::kTxPowerMinDecisionIntervalMs) {
    return decision;
  }

  // No margin samples means no basis for a decision. Happens on a node that has
  // sent STATUS but whose frames all failed to decode, and right after a reset.
  if (node.snrCount == 0) {
    return decision;
  }

  const int16_t avgSnrDbX10 =
      static_cast<int16_t>((node.snrSumDb * 10) / static_cast<int32_t>(node.snrCount));
  const int16_t marginDbX10 =
      static_cast<int16_t>(avgSnrDbX10 - BaseConfig::kSnrDemodFloorDbX10);
  decision.marginDbX10 = marginDbX10;

  // Deltas span the whole decision interval, not the gap between two STATUS
  // packets — which is the point of pacing on the controller's own clock.
  // Unsigned wrap-around arithmetic is correct here: these are u16 counters
  // that saturate at 65535 on the node, and a reboot (the only thing that
  // resets them) clears haveLinkStats via onAwaken() before this runs.
  const uint16_t retxDelta = static_cast<uint16_t>(retxTotal - node.lastRetxTotal);
  const uint16_t failDelta = static_cast<uint16_t>(failTotal - node.lastFailTotal);

  // The decision window closes here — margin samples, link-stat baseline, and
  // the rate-limit clock all reset together. Keeping them in one place is what
  // stops a "no change" outcome from silently leaving one of them stale.
  node.haveDecided = true;
  node.lastDecisionMs = nowMs;
  node.snrSumDb = 0;
  node.snrCount = 0;
  node.lastRetxTotal = retxTotal;
  node.lastFailTotal = failTotal;

  // --- Up: margin is under target -------------------------------------------
  // Checked first and unconditionally, ahead of the retry inhibitor below: a
  // node that is both short on margin *and* retrying is the case most in need
  // of more power, not least.
  //
  // Recovery is a single jump to baseline rather than a step. The costs are not
  // symmetric — an extra few dB costs a sliver of battery, while a few dB short
  // costs telemetry plus the retries to recover it, which burn more energy than
  // the reduction ever saved. Stepping up would also mean an edge node spends
  // several decision intervals still under-powered while it feels its way back.
  if (marginDbX10 < BaseConfig::kTargetSnrMarginDbX10) {
    const int8_t target = NetworkConfig::kMaxTxPowerDbm;
    if (target != node.currentDbm) {
      decision.action = Action::SetPower;
      decision.targetDbm = target;
      decision.reason = Reason::MarginLow;
    }
    return decision;
  }

  // --- Down: only from a comfortable, quiet interval -------------------------
  //
  // Retries inhibit a step-down but never trigger a step-up. Under
  // AppLayerAckSummary a retransmit means the node did not see its packet
  // confirmed in an ACK_SUMMARY, which depends on the *downlink* — so a high
  // retry count with healthy margin usually indicts the base's transmitter, not
  // the node's, and raising node power would spend battery on the wrong link.
  // In Timed mode it is murkier still: the base deliberately defers acks across
  // standby and a single lost window marker costs a retransmission by design,
  // so retries have a duty-cycle-structural floor unrelated to link quality.
  //
  // That makes the signal trustworthy in exactly one direction — nonzero
  // retries are decent evidence the node is *not* comfortable, and no evidence
  // at all about whose fault it is. So it gates the optimisation and nothing
  // more. Using it as an alarm threshold would mean recalibrating against the
  // duty cycle every time the window period changed.
  if (retxDelta > 0 || failDelta > 0) {
    return decision;
  }

  if (marginDbX10 <=
      static_cast<int16_t>(BaseConfig::kTargetSnrMarginDbX10 + BaseConfig::kSnrDeadBandDbX10)) {
    return decision;  // inside the dead band — leave it alone
  }

  const int8_t target =
      clampDbm(static_cast<int32_t>(node.currentDbm) - BaseConfig::kTxPowerStepDbm);
  if (target != node.currentDbm) {
    decision.action = Action::SetPower;
    decision.targetDbm = target;
    decision.reason = Reason::HeadroomStepDown;
  }
  return decision;
}

void TxPowerController::onCmdAck(uint8_t nodeId, uint32_t nowMs) {
  Node *node = find(nodeId);
  if (node == nullptr || !node->hasPending) {
    return;
  }

  node->currentDbm = node->pendingDbm;
  node->hasPending = false;
  node->heard = true;
  node->lastHeardMs = nowMs;
}

void TxPowerController::onAwaken(uint8_t nodeId, uint32_t nowMs) {
  Node *node = findOrCreate(nodeId);
  if (node == nullptr) {
    return;
  }

  const uint8_t id = node->nodeId;
  *node = Node{};
  node->inUse = true;
  node->nodeId = id;
  node->heard = true;
  node->lastHeardMs = nowMs;
  // Everything else returns to its constructed default: baseline power, DYNAMIC
  // mode, no link-stat history. That is not a convenience — a rebooted node
  // restarts retx_total/fail_total at zero, so carrying the old totals over
  // would produce a wildly negative delta on the next STATUS. The mode reset is
  // deliberate too: an operator's STATIC pin does not survive the node
  // forgetting it, and the base pretending otherwise would leave the two
  // disagreeing with no way to notice.
}

void TxPowerController::setMode(uint8_t nodeId, uint8_t mode) {
  Node *node = findOrCreate(nodeId);
  if (node == nullptr) {
    return;
  }
  node->mode = (mode == BinaryPacket::TX_POWER_MODE_STATIC)
                   ? BinaryPacket::TX_POWER_MODE_STATIC
                   : BinaryPacket::TX_POWER_MODE_DYNAMIC;
}

TxPowerController::Decision TxPowerController::update(uint32_t nowMs) {
  Decision decision;

  for (auto &node : _nodes) {
    if (!node.inUse) {
      continue;
    }

    // Expire an unacked command rather than assuming it landed. Sized to
    // outlast a full duty cycle — see BaseConfig::kCmdAckTimeoutMs.
    if (node.hasPending &&
        (nowMs - node.pendingSinceMs) >= BaseConfig::kCmdAckTimeoutMs) {
      node.hasPending = false;
      // currentDbm is deliberately left as it was: the command may or may not
      // have been applied, and the next STATUS from the node settles it.
      // Because commands are absolute, being wrong here is never dangerous.
    }

    if (decision.action != Action::None) {
      continue;  // already have one to return; keep expiring the rest
    }

    // Silence probe — the uplink-dead case. The node cannot trip its own
    // stale-sync revert while its downlink still works, so the base is the only
    // actor that can restore it, and it must do so blind.
    if (node.mode == BinaryPacket::TX_POWER_MODE_STATIC || node.hasPending ||
        !node.heard) {
      continue;
    }
    if ((nowMs - node.lastHeardMs) < BaseConfig::kTxPowerSilenceTimeoutMs) {
      continue;
    }
    if (node.silenceProbes >= BaseConfig::kMaxTxPowerSilenceProbes) {
      continue;
    }
    if (node.currentDbm == NetworkConfig::kMaxTxPowerDbm) {
      continue;  // already at baseline — a probe could not change anything
    }

    decision.action = Action::SetPower;
    decision.nodeId = node.nodeId;
    decision.targetDbm = NetworkConfig::kMaxTxPowerDbm;
    decision.reason = Reason::SilenceProbe;
    node.silenceProbes++;
  }

  return decision;
}

void TxPowerController::onCommandSent(uint8_t nodeId, int8_t targetDbm,
                                      uint32_t nowMs) {
  Node *node = find(nodeId);
  if (node == nullptr) {
    return;
  }
  node->hasPending = true;
  node->pendingDbm = targetDbm;
  node->pendingSinceMs = nowMs;
}

void TxPowerController::onCommandFailed(uint8_t nodeId) {
  Node *node = find(nodeId);
  if (node == nullptr) {
    return;
  }
  // Explicitly does NOT arm the in-flight gate. A command that never made it
  // onto the queue is not "in flight", and treating it as such would stall this
  // node's decisions for a whole ack timeout for no reason.
  node->hasPending = false;
}

bool TxPowerController::known(uint8_t nodeId) const {
  return find(nodeId) != nullptr;
}

int8_t TxPowerController::currentDbm(uint8_t nodeId) const {
  const Node *node = find(nodeId);
  return node ? node->currentDbm : NetworkConfig::kRadioTxPowerDbm;
}

bool TxPowerController::isStatic(uint8_t nodeId) const {
  const Node *node = find(nodeId);
  return node && node->mode == BinaryPacket::TX_POWER_MODE_STATIC;
}

const char *TxPowerController::reasonName(Reason reason) {
  switch (reason) {
  case Reason::MarginLow:
    return "margin_low";
  case Reason::HeadroomStepDown:
    return "headroom_step_down";
  case Reason::SilenceProbe:
    return "silence_probe";
  case Reason::None:
  default:
    return "none";
  }
}
