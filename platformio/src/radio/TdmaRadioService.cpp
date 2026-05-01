#include "radio/TdmaRadioService.h"

#include "telemetry/BinaryPacket.h"

#include <Arduino.h>
#include <string.h>

namespace {

bool telemetryUsesLinkAck(const TdmaConfig &cfg) {
  switch (cfg.reliabilityMode) {
    case TdmaReliabilityMode::StrictLinkAck:
      return cfg.enableLinkAck;
    case TdmaReliabilityMode::AppLayerAckSummary:
      return false;
  }

  return cfg.enableLinkAck;
}

static const char* pktTypeName(uint8_t pktType) {
  switch (pktType) {
    case BinaryPacket::PKT_AWAKEN:      return "AWAKEN";
    case BinaryPacket::PKT_BUNDLE:      return "BUNDLE";
    case BinaryPacket::PKT_STATUS:      return "STATUS";
    case BinaryPacket::PKT_FULL_STATE:  return "FULL_STATE";
    case BinaryPacket::PKT_TIME_SYNC:   return "TIME_SYNC";
    case BinaryPacket::PKT_ACK_SUMMARY: return "ACK_SUMMARY";
    default:                            return "UNKNOWN";
  }
}

bool decodeHeader(const uint8_t *payload,
                  uint8_t len,
                  BinaryPacket::PktHeader &hdrOut) {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    return false;
  }

  memcpy(&hdrOut, payload, sizeof(BinaryPacket::PktHeader));
  return hdrOut.magic == BinaryPacket::PKT_MAGIC;
}

void printQueueSnapshot(uint8_t queuedCount,
                        uint8_t queueCapacity,
                        uint8_t pendingCount,
                        uint32_t droppedOldestCount) {
  Serial.print("  q=");
  Serial.print(queuedCount);
  Serial.print('/');
  Serial.print(queueCapacity);
  Serial.print("  pending=");
  Serial.print(pendingCount);
  Serial.print("  dropped=");
  Serial.print(droppedOldestCount);
}

// Conservative slot-budget estimates to prevent crossing slot boundaries.
// Values include airtime plus software/radio overhead margin.
uint16_t estimateTxBudgetMs(const uint8_t *payload, uint8_t len) {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    return 140;
  }

  BinaryPacket::PktHeader hdr;
  memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));

  if (hdr.magic != BinaryPacket::PKT_MAGIC) {
    return 140;
  }

  switch (hdr.pkt_type) {
    case BinaryPacket::PKT_BUNDLE:
      return 340;
    case BinaryPacket::PKT_STATUS:
      return 120;
    case BinaryPacket::PKT_AWAKEN:
      return 90;
    case BinaryPacket::PKT_FULL_STATE:
      return 140;
    default:
      return 140;
  }
}

} // namespace

TdmaRadioService::TdmaRadioService(const TdmaConfig &cfg,
                                   TdmaClock &tdmaClock,
                                   TdmaTxQueue &queue,
                                   ITdmaRadioDriver &driver)
    : _cfg(cfg), _tdmaClock(tdmaClock), _queue(queue), _driver(driver) {}

bool TdmaRadioService::begin() {
  _error = TdmaRadioError::None;

  if (!_driver.begin()) {
    _state = TdmaRadioState::Error;
    _error = TdmaRadioError::BeginFailed;
    return false;
  }

  _state = TdmaRadioState::Ready;
  return true;
}

void TdmaRadioService::update() {
  if (_state != TdmaRadioState::Ready) {
    return;
  }

  checkIncomingTimeSync();
  drainTxQueue();
}

bool TdmaRadioService::sendAwakenHandshake(const uint8_t *payload, uint8_t len) {
  if (_state != TdmaRadioState::Ready) {
    return false;
  }

  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodeHeader(payload, len, hdr);
  if (!hasHdr || hdr.pkt_type != BinaryPacket::PKT_AWAKEN) {
    Serial.print("[Radio][AWAKEN_DIRECT] REJECT ");
    Serial.print(hasHdr ? pktTypeName(hdr.pkt_type) : "RAW");
    Serial.print(" seq=");
    Serial.print(hasHdr ? hdr.seq : 0);
    Serial.println(" reason=not_awaken_packet");
    return false;
  }

  const bool ok = _driver.sendToWait(payload, len, _cfg.baseAddr);

  if (!ok) {
    _error = TdmaRadioError::SendFailed;
  }

  Serial.print(ok ? "[Radio][AWAKEN_DIRECT] SENT " : "[Radio][AWAKEN_DIRECT] FAIL ");
  Serial.print(hasHdr ? pktTypeName(hdr.pkt_type) : "RAW");
  Serial.print(" seq=");
  Serial.print(hasHdr ? hdr.seq : 0);
  Serial.print(" link_ack=");
  Serial.println(ok ? "OK" : "NO");

  return ok;
}

bool TdmaRadioService::enqueueTelemetry(const uint8_t *payload, uint8_t len) {
  if (_state != TdmaRadioState::Ready) {
    return false;
  }

  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodeHeader(payload, len, hdr);
  if (hasHdr && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
    Serial.print("[Radio][ENQ_REJECT] ");
    Serial.print(hasHdr ? pktTypeName(hdr.pkt_type) : "RAW");
    Serial.print(" seq=");
    Serial.print(hasHdr ? hdr.seq : 0);
    Serial.println(" reason=awaken_handshake_only");
    return false;
  }

  const uint32_t droppedBefore = _queue.droppedOldestCount();
  if (!_queue.enqueue(payload, len)) {
    _error = TdmaRadioError::EnqueueFailed;
    return false;
  }

  _enqueuedCount++;

  if (_queue.droppedOldestCount() != droppedBefore) {
    Serial.print("[Radio][DROP#");
    Serial.print(_queue.droppedOldestCount());
    Serial.print("] DROP_OLDEST cause=queue_full incoming=");
    Serial.print(hasHdr ? pktTypeName(hdr.pkt_type) : "RAW");
    Serial.print(" seq=");
    Serial.print(hasHdr ? hdr.seq : 0);
    printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                       _queue.droppedOldestCount());
    Serial.println();
  }

  Serial.print("[Radio][ENQ#");
  Serial.print(_enqueuedCount);
  Serial.print("] ");
  Serial.print(hasHdr ? pktTypeName(hdr.pkt_type) : "RAW");
  Serial.print(" seq=");
  Serial.print(hasHdr ? hdr.seq : 0);
  printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                     _queue.droppedOldestCount());
  Serial.println();

  return true;
}

uint8_t TdmaRadioService::nodeId() const {
  return _cfg.nodeId;
}

uint8_t TdmaRadioService::numSlots() const {
  return _cfg.numSlots;
}

TdmaRadioState TdmaRadioService::state() const {
  return _state;
}

TdmaRadioError TdmaRadioService::error() const {
  return _error;
}

uint8_t TdmaRadioService::queuedCount() const {
  return _queue.count();
}

uint32_t TdmaRadioService::sentCount() const {
  return _sentCount;
}

uint32_t TdmaRadioService::failedSendCount() const {
  return _failedSendCount;
}

uint32_t TdmaRadioService::droppedOldestCount() const {
  return _queue.droppedOldestCount();
}

uint32_t TdmaRadioService::lastTxSlotIndex() const {
  return _lastTxSlotIndex;
}

void TdmaRadioService::drainTxQueue() {
  dropExpiredPending();

  const bool hasFreshSync = _tdmaClock.hasSync() && !_tdmaClock.syncStale();
  const bool useAppReliability = _cfg.enableAppReliability && hasFreshSync;

  uint32_t slotIndex = 0;

  if (!_tdmaClock.myTurn(slotIndex)) {
    return;
  }

  _lastTxSlotIndex = slotIndex;

  const bool useSlotBudget = hasFreshSync;
  const uint32_t slotEndMs = (_cfg.slotWidthMs > _cfg.guardMs) ? (_cfg.slotWidthMs - _cfg.guardMs) : _cfg.slotWidthMs;
  // For transmission debugging, keep sends serialized so every ACK exchange is easy to follow.
  const uint8_t kMaxSendsPerUpdate = 1;

  uint8_t sendsThisUpdate = 0;

  while (sendsThisUpdate < kMaxSendsPerUpdate) {
    if (useSlotBudget) {
      const uint32_t posInSlot = _tdmaClock.positionInSlotMs();
      if (posInSlot >= slotEndMs) {
        return;
      }
    } else if (sendsThisUpdate > 0) {
      // In unsynced/stale fallback mode, keep behavior conservative.
      return;
    }

    uint8_t payload[TdmaConfig::MaxPayloadLen] = {};
    uint8_t len = 0;
    bool fromQueue = false;
    uint8_t pendingIndex = 0;
    uint8_t retrySeq = 0;

    if (!_queue.empty()) {
      if (!_queue.dequeue(payload, len)) {
        return;
      }
      fromQueue = true;
    } else if (useAppReliability) {
      if (!pickRetransmitCandidate(payload, len, retrySeq, pendingIndex)) {
        return;
      }
    } else {
      return;
    }

    if (useSlotBudget) {
      const uint32_t posInSlot = _tdmaClock.positionInSlotMs();
      const uint32_t remainingMs = (slotEndMs > posInSlot) ? (slotEndMs - posInSlot) : 0;
      const uint16_t neededMs = estimateTxBudgetMs(payload, len);
      if (remainingMs < neededMs) {
        // Not enough safe budget left for this payload.
        if (fromQueue) {
          const uint32_t droppedBefore = _queue.droppedOldestCount();
          const bool enqOk = _queue.enqueue(payload, len);
          if (!enqOk) {
            _error = TdmaRadioError::EnqueueFailed;
          } else {
            BinaryPacket::PktHeader deferHdr = {};
            const bool hasDeferHdr = decodeHeader(payload, len, deferHdr);
            if (_queue.droppedOldestCount() != droppedBefore) {
              Serial.print("[Radio][DROP#");
              Serial.print(_queue.droppedOldestCount());
              Serial.print("] DROP_OLDEST cause=slot_defer_requeue incoming=");
              Serial.print(hasDeferHdr ? pktTypeName(deferHdr.pkt_type) : "RAW");
              Serial.print(" seq=");
              Serial.print(hasDeferHdr ? deferHdr.seq : 0);
              printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                                 _queue.droppedOldestCount());
              Serial.println();
            }
          }
        }
        return;
      }
    }

    BinaryPacket::PktHeader hdr = {};
    if (len >= sizeof(BinaryPacket::PktHeader)) {
      memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));
    }
    const bool useLinkAck = telemetryUsesLinkAck(_cfg);

    const uint16_t attemptCountWide = static_cast<uint16_t>(_cfg.maxRetries) + 1u;
    const uint8_t maxAttempts =
        attemptCountWide > 0xFFu ? 0xFFu : static_cast<uint8_t>(attemptCountWide);
    bool ok = false;
    uint8_t attemptsUsed = 0;

    if (!useLinkAck) {
      Serial.print("[Radio][TX_NOACK] ");
      Serial.print(pktTypeName(hdr.pkt_type));
      Serial.print(" seq=");
      Serial.print(hdr.seq);
      Serial.print(" slot=");
      Serial.println(slotIndex);
      ok = _driver.send(payload, len, _cfg.baseAddr);
    }

    for (uint8_t attempt = 1; useLinkAck && attempt <= maxAttempts; ++attempt) {
      attemptsUsed = attempt;
      Serial.print("[Radio][LINK_ACK_TX] ");
      Serial.print(pktTypeName(hdr.pkt_type));
      Serial.print(" seq=");
      Serial.print(hdr.seq);
      Serial.print(" slot=");
      Serial.print(slotIndex);
      Serial.print(" attempt=");
      Serial.print(attempt);
      Serial.print('/');
      Serial.print(maxAttempts);
      Serial.print(" timeout_ms=");
      Serial.print(_cfg.ackTimeoutMs);
      Serial.println();

      if (_driver.sendToWait(payload, len, _cfg.baseAddr)) {
        ok = true;
        Serial.print("[Radio][LINK_ACK_RX] ACK_OK seq=");
        Serial.print(hdr.seq);
        Serial.print(" attempt=");
        Serial.print(attempt);
        Serial.print(" retries_used=");
        Serial.println(attempt - 1);
        break;
      }

      Serial.print("[Radio][LINK_ACK_RX] ACK_TIMEOUT seq=");
      Serial.print(hdr.seq);
      Serial.print(" attempt=");
      Serial.print(attempt);
      Serial.print(" retries_remaining=");
      Serial.println(maxAttempts - attempt);
    }

    if (ok) {
      _sentCount++;
      if (useAppReliability) {
        if (fromQueue) {
          rememberSentTelemetry(payload, len);
          Serial.print("[Radio][TX#");
          Serial.print(_sentCount);
          Serial.print("] SENT ");
          Serial.print(pktTypeName(hdr.pkt_type));
          Serial.print(" seq=");
          Serial.print(hdr.seq);
          Serial.print(" slot=");
          Serial.print(slotIndex);
          Serial.print(" link_ack=OK");
          Serial.print(" retries_used=");
          Serial.print(attemptsUsed > 0 ? attemptsUsed - 1 : 0);
          printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                             _queue.droppedOldestCount());
          Serial.println();
        } else {
          _retransmitCount++;
          Serial.print("[Radio][RTX#");
          Serial.print(_retransmitCount);
          Serial.print("] RETX ");
          Serial.print(pktTypeName(hdr.pkt_type));
          Serial.print(" seq=");
          Serial.print(hdr.seq);
          Serial.print(" attempt=");
          Serial.print(_pending[pendingIndex].attempts + 1);
          Serial.print(" slot=");
          Serial.print(slotIndex);
          Serial.print(" retries_used=");
          Serial.print(attemptsUsed > 0 ? attemptsUsed - 1 : 0);
          printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                             _queue.droppedOldestCount());
          Serial.println();
          markRetransmitSent(pendingIndex);
        }
      } else {
        Serial.print("[Radio][TX#");
        Serial.print(_sentCount);
        Serial.print("] SENT ");
        Serial.print(pktTypeName(hdr.pkt_type));
        Serial.print(" seq=");
        Serial.print(hdr.seq);
        Serial.print(" slot=");
        Serial.print(slotIndex);
        printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                           _queue.droppedOldestCount());
        Serial.println();
      }
    } else {
      _failedSendCount++;
      _error = TdmaRadioError::SendFailed;
      Serial.print("[Radio][FAIL#");
      Serial.print(_failedSendCount);
      Serial.print("] FAIL ");
      Serial.print(pktTypeName(hdr.pkt_type));
      Serial.print(" seq=");
      Serial.print(hdr.seq);
      Serial.print(" link_ack=NO");
      Serial.print(" retries_used=");
      Serial.print(attemptsUsed > 0 ? attemptsUsed - 1 : _cfg.maxRetries);
        if (!fromQueue && useAppReliability && pendingIndex < kMaxReliabilityWindow &&
          _pending[pendingIndex].inUse) {
        Serial.print(" attempt=");
        Serial.print(_pending[pendingIndex].attempts + 1);
      }
      printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                         _queue.droppedOldestCount());
      Serial.println();

      // Match original behavior for fresh telemetry: failed send is dropped, not requeued.
      // Retransmit entries are retained unless expired/attempt-limited.
      if (fromQueue) {
        Serial.print("[Radio][DROP#");
        Serial.print(_queue.droppedOldestCount() + _pendingDropCount + 1);
        Serial.print("] DROP_TX_FAIL ");
        Serial.print(pktTypeName(hdr.pkt_type));
        Serial.print(" seq=");
        Serial.print(hdr.seq);
        Serial.print(" reason=send_failed");
        printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                           _queue.droppedOldestCount());
        Serial.println();
      }
    }

    sendsThisUpdate++;

    if (_queue.empty() && !useAppReliability) {
      return;
    }
  }
}

void TdmaRadioService::checkIncomingTimeSync() {
  while (_driver.available()) {
    ITdmaRadioDriver::ReceivedPacket packet;

    if (!_driver.receive(packet)) {
      return;
    }

    uint32_t sessionMs = 0;
    uint8_t assignedNodeId = 0;
    BinaryPacket::AckSummaryPayload ack = {};

    if (isTimeSyncPacket(packet, sessionMs, assignedNodeId)) {
      if (assignedNodeId != 0 && !applyAssignedNodeId(assignedNodeId)) {
        Serial.print("[Radio][SYNC] IGNORE node=");
        Serial.print(assignedNodeId);
        Serial.println(" reason=assignment_apply_failed");
        continue;
      }

      _tdmaClock.applySync(sessionMs);
      _timeSyncCount++;
      Serial.print("[Radio][SYNC#");
      Serial.print(_timeSyncCount);
      Serial.print("] TIME_SYNC rcv sessionMs=");
      Serial.print(sessionMs);
      Serial.print(" node=");
      Serial.println(_cfg.nodeId);
      continue;
    }

    if (_cfg.enableAppReliability && isAckSummaryPacket(packet, ack)) {
      _ackSummaryCount++;
      Serial.print("[Radio][ACK#");
      Serial.print(_ackSummaryCount);
      Serial.print("] ACK_SUMMARY rcv node=");
      Serial.print(ack.node_id);
      Serial.print(" base_seq=");
      Serial.print(ack.ack_base_seq);
      Serial.print(" mask=0x");
      Serial.println(ack.ack_mask, HEX);
      applyAckSummary(ack);
    }
  }
}

bool TdmaRadioService::isTimeSyncPacket(
    const ITdmaRadioDriver::ReceivedPacket &packet,
    uint32_t &sessionMsOut,
    uint8_t &assignedNodeIdOut) const {
  BinaryPacket::PktHeader hdr;
  BinaryPacket::TimeSyncPayload ts;

  if (!BinaryPacket::decodeTimeSync(packet.data, packet.len, hdr, ts)) {
    return false;
  }

  if (_cfg.nodeId == 0) {
    if (hdr.node_id == 0) {
      return false;
    }
    assignedNodeIdOut = hdr.node_id;
  } else {
    if (hdr.node_id != 0 && hdr.node_id != _cfg.nodeId) {
      return false;
    }
    assignedNodeIdOut = hdr.node_id;
  }

  sessionMsOut = ts.session_time_ms;
  return true;
}

bool TdmaRadioService::applyAssignedNodeId(uint8_t nodeId) {
  if (nodeId == 0) {
    return false;
  }

  if (_cfg.nodeId == nodeId) {
    return true;
  }

  if (!_driver.setLocalAddress(nodeId)) {
    return false;
  }

  _cfg.nodeId = nodeId;
  _tdmaClock.applyAssignment(nodeId, _cfg.numSlots);
  return true;
}

bool TdmaRadioService::isAckSummaryPacket(
    const ITdmaRadioDriver::ReceivedPacket &packet,
    BinaryPacket::AckSummaryPayload &ackOut) const {
  BinaryPacket::PktHeader hdr;
  return BinaryPacket::decodeAckSummary(packet.data, packet.len, hdr, ackOut);
}

bool TdmaRadioService::isTelemetryPacketForNode(const uint8_t *payload,
                                                uint8_t len,
                                                uint8_t &seqOut) const {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    return false;
  }

  BinaryPacket::PktHeader hdr;
  memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));

  if (hdr.magic != BinaryPacket::PKT_MAGIC || hdr.node_id != _cfg.nodeId) {
    return false;
  }

  const bool telemetryType =
      hdr.pkt_type == BinaryPacket::PKT_BUNDLE ||
      hdr.pkt_type == BinaryPacket::PKT_STATUS ||
      hdr.pkt_type == BinaryPacket::PKT_FULL_STATE;

  if (!telemetryType) {
    return false;
  }

  seqOut = hdr.seq;
  return true;
}

void TdmaRadioService::rememberSentTelemetry(const uint8_t *payload, uint8_t len) {
  uint8_t seq = 0;
  if (!isTelemetryPacketForNode(payload, len, seq)) {
    return;
  }

  const uint8_t windowDepth =
      (_cfg.reliabilityWindowDepth > kMaxReliabilityWindow)
          ? kMaxReliabilityWindow
          : _cfg.reliabilityWindowDepth;
  if (windowDepth == 0) {
    return;
  }

  const uint32_t nowMs = _tdmaClock.sessionNowMs();

  int8_t freeIndex = -1;
  int8_t oldestIndex = -1;
  uint32_t oldestSentMs = 0;
  uint8_t replacedSeq = 0;
  bool replacingPending = false;

  for (uint8_t i = 0; i < windowDepth; ++i) {
    PendingEntry &e = _pending[i];
    if (!e.inUse) {
      if (freeIndex < 0) {
        freeIndex = static_cast<int8_t>(i);
      }
      continue;
    }

    if (e.seq == seq) {
      memcpy(e.payload, payload, len);
      e.len = len;
      e.lastSentMs = nowMs;
      e.attempts = 1;
      return;
    }

    if (oldestIndex < 0 || e.firstSentMs < oldestSentMs) {
      oldestIndex = static_cast<int8_t>(i);
      oldestSentMs = e.firstSentMs;
      replacedSeq = e.seq;
    }
  }

  int8_t slot = freeIndex;
  if (slot < 0) {
    slot = oldestIndex;
  }
  if (slot < 0) {
    return;
  }

  PendingEntry &e = _pending[slot];
  replacingPending = e.inUse;
  if (!e.inUse) {
    _pendingCount++;
  }

  e.inUse = true;
  e.seq = seq;
  memcpy(e.payload, payload, len);
  e.len = len;
  e.firstSentMs = nowMs;
  e.lastSentMs = nowMs;
  e.attempts = 1;

  if (replacingPending) {
    _pendingDropCount++;
    Serial.print("[Radio][DROP#");
    Serial.print(_queue.droppedOldestCount() + _pendingDropCount);
    Serial.print("] DROP_PENDING_WINDOW old_seq=");
    Serial.print(replacedSeq);
    Serial.print(" new_seq=");
    Serial.print(seq);
    printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                       _queue.droppedOldestCount());
    Serial.println();
  }
}

bool TdmaRadioService::pickRetransmitCandidate(uint8_t *payloadOut,
                                               uint8_t &lenOut,
                                               uint8_t &seqOut,
                                               uint8_t &pendingIndexOut) {
  const uint8_t windowDepth =
      (_cfg.reliabilityWindowDepth > kMaxReliabilityWindow)
          ? kMaxReliabilityWindow
          : _cfg.reliabilityWindowDepth;
  if (windowDepth == 0 || !payloadOut) {
    lenOut = 0;
    return false;
  }

  const uint32_t nowMs = _tdmaClock.sessionNowMs();

  int8_t bestIndex = -1;
  uint32_t oldestLastSentMs = 0;

  for (uint8_t i = 0; i < windowDepth; ++i) {
    PendingEntry &e = _pending[i];
    if (!e.inUse) {
      continue;
    }

    const uint32_t ageMs = nowMs - e.firstSentMs;
    if (ageMs > _cfg.reliabilityMaxAgeMs || e.attempts >= _cfg.reliabilityMaxAttempts) {
      _pendingDropCount++;
      Serial.print("[Radio][DROP#");
      Serial.print(_queue.droppedOldestCount() + _pendingDropCount);
      Serial.print("] DROP_PENDING seq=");
      Serial.print(e.seq);
      Serial.print(" reason=");
      Serial.print(ageMs > _cfg.reliabilityMaxAgeMs ? "max_age" : "max_attempts");
      Serial.print(" attempts=");
      Serial.print(e.attempts);
      Serial.print(" age_ms=");
      Serial.print(ageMs);
      printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                         _queue.droppedOldestCount());
      Serial.println();
      e.inUse = false;
      if (_pendingCount > 0) {
        _pendingCount--;
      }
      continue;
    }

    if ((nowMs - e.lastSentMs) < _cfg.reliabilityMinRetryGapMs) {
      continue;
    }

    if (bestIndex < 0 || e.lastSentMs < oldestLastSentMs) {
      bestIndex = static_cast<int8_t>(i);
      oldestLastSentMs = e.lastSentMs;
    }
  }

  if (bestIndex < 0) {
    lenOut = 0;
    return false;
  }

  PendingEntry &e = _pending[bestIndex];
  memcpy(payloadOut, e.payload, e.len);
  lenOut = e.len;
  seqOut = e.seq;
  pendingIndexOut = static_cast<uint8_t>(bestIndex);
  return true;
}

void TdmaRadioService::markRetransmitSent(uint8_t pendingIndex) {
  if (pendingIndex >= kMaxReliabilityWindow) {
    return;
  }

  PendingEntry &e = _pending[pendingIndex];
  if (!e.inUse) {
    return;
  }

  e.lastSentMs = _tdmaClock.sessionNowMs();
  if (e.attempts < 0xFF) {
    e.attempts++;
  }
}

void TdmaRadioService::applyAckSummary(const BinaryPacket::AckSummaryPayload &ack) {
  if (ack.node_id != _cfg.nodeId) {
    return;
  }

  const uint8_t windowDepth =
      (_cfg.reliabilityWindowDepth > kMaxReliabilityWindow)
          ? kMaxReliabilityWindow
          : _cfg.reliabilityWindowDepth;

  for (uint8_t i = 0; i < windowDepth; ++i) {
    PendingEntry &e = _pending[i];
    if (!e.inUse) {
      continue;
    }

    bool acked = false;

    // If (ack_base_seq - seq) is < 128 in modulo arithmetic, seq is older/equal.
    if (static_cast<uint8_t>(ack.ack_base_seq - e.seq) < 128u) {
      acked = true;
    } else {
      const uint8_t ahead = static_cast<uint8_t>(e.seq - ack.ack_base_seq);
      if (ahead >= 1 && ahead <= 16) {
        acked = ((ack.ack_mask >> (ahead - 1)) & 0x01u) != 0u;
      }
    }

    if (acked) {
      Serial.print("[Radio][ACK] ACKED seq=");
      Serial.print(e.seq);
      Serial.print(" attempts=");
      Serial.print(e.attempts);
      Serial.print(" base_seq=");
      Serial.print(ack.ack_base_seq);
      Serial.print(" mask=0x");
      Serial.print(ack.ack_mask, HEX);
      printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                         _queue.droppedOldestCount());
      Serial.println();
      e.inUse = false;
      if (_pendingCount > 0) {
        _pendingCount--;
      }
    } else {
      const uint32_t ageMs = _tdmaClock.sessionNowMs() - e.firstSentMs;
      Serial.print("[Radio][ACK] NEEDS_RETX seq=");
      Serial.print(e.seq);
      Serial.print(" next_attempt=");
      Serial.print(e.attempts + 1);
      Serial.print(" age_ms=");
      Serial.print(ageMs);
      Serial.print(" base_seq=");
      Serial.print(ack.ack_base_seq);
      Serial.print(" mask=0x");
      Serial.print(ack.ack_mask, HEX);
      printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                         _queue.droppedOldestCount());
      Serial.println();
    }
  }
}

void TdmaRadioService::dropExpiredPending() {
  if (!_cfg.enableAppReliability) {
    return;
  }

  const uint8_t windowDepth =
      (_cfg.reliabilityWindowDepth > kMaxReliabilityWindow)
          ? kMaxReliabilityWindow
          : _cfg.reliabilityWindowDepth;
  const uint32_t nowMs = _tdmaClock.sessionNowMs();

  for (uint8_t i = 0; i < windowDepth; ++i) {
    PendingEntry &e = _pending[i];
    if (!e.inUse) {
      continue;
    }

    const uint32_t ageMs = nowMs - e.firstSentMs;
    if (ageMs > _cfg.reliabilityMaxAgeMs || e.attempts >= _cfg.reliabilityMaxAttempts) {
      _pendingDropCount++;
      Serial.print("[Radio][DROP#");
      Serial.print(_queue.droppedOldestCount() + _pendingDropCount);
      Serial.print("] DROP_PENDING seq=");
      Serial.print(e.seq);
      Serial.print(" reason=");
      Serial.print(ageMs > _cfg.reliabilityMaxAgeMs ? "max_age" : "max_attempts");
      Serial.print(" attempts=");
      Serial.print(e.attempts);
      Serial.print(" age_ms=");
      Serial.print(ageMs);
      printQueueSnapshot(_queue.count(), _queue.capacity(), _pendingCount,
                         _queue.droppedOldestCount());
      Serial.println();
      e.inUse = false;
      if (_pendingCount > 0) {
        _pendingCount--;
      }
    }
  }
}
