#include "radio/TdmaRadioService.h"

#include "telemetry/BinaryPacket.h"

#include <Arduino.h>
#include <string.h>

namespace {

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

bool TdmaRadioService::enqueueTelemetry(const uint8_t *payload, uint8_t len) {
  if (_state != TdmaRadioState::Ready) {
    return false;
  }

  if (!_queue.enqueue(payload, len)) {
    _error = TdmaRadioError::EnqueueFailed;
    return false;
  }

  return true;
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

  uint32_t slotIndex = 0;

  if (!_tdmaClock.myTurn(slotIndex)) {
    return;
  }

  _lastTxSlotIndex = slotIndex;

  const bool useSlotBudget = _tdmaClock.hasSync() && !_tdmaClock.syncStale();
  const uint32_t slotEndMs = (_cfg.slotWidthMs > _cfg.guardMs) ? (_cfg.slotWidthMs - _cfg.guardMs) : _cfg.slotWidthMs;
  const uint8_t kMaxSendsPerUpdate = 6;

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
    } else if (_cfg.enableAppReliability) {
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
          const bool enqOk = _queue.enqueue(payload, len);
          if (!enqOk) {
            _error = TdmaRadioError::EnqueueFailed;
          }
        }
        return;
      }
    }

    BinaryPacket::PktHeader hdr = {};
    if (len >= sizeof(BinaryPacket::PktHeader)) {
      memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));
    }

    const bool ok = _cfg.enableAppReliability
                        ? _driver.send(payload, len, _cfg.baseAddr)
                        : _driver.sendToWait(payload, len, _cfg.baseAddr);

    if (ok) {
      _sentCount++;
      if (_cfg.enableAppReliability) {
        if (fromQueue) {
          rememberSentTelemetry(payload, len);
          Serial.print("[Radio] SENT ");
          Serial.print(pktTypeName(hdr.pkt_type));
          Serial.print("  seq=");
          Serial.print(hdr.seq);
          Serial.print("  slot=");
          Serial.println(slotIndex);
        } else {
          Serial.print("[Radio] RETX ");
          Serial.print(pktTypeName(hdr.pkt_type));
          Serial.print("  seq=");
          Serial.print(hdr.seq);
          Serial.print("  attempt=");
          Serial.print(_pending[pendingIndex].attempts + 1);
          Serial.print("  slot=");
          Serial.println(slotIndex);
          markRetransmitSent(pendingIndex);
        }
      } else {
        Serial.print("[Radio] SENT ");
        Serial.print(pktTypeName(hdr.pkt_type));
        Serial.print("  seq=");
        Serial.print(hdr.seq);
        Serial.print("  slot=");
        Serial.println(slotIndex);
      }
    } else {
      _failedSendCount++;
      _error = TdmaRadioError::SendFailed;
      Serial.print("[Radio] FAIL ");
      Serial.print(pktTypeName(hdr.pkt_type));
      Serial.print("  seq=");
      Serial.println(hdr.seq);

      // Match original behavior for fresh telemetry: failed send is dropped, not requeued.
      // Retransmit entries are retained unless expired/attempt-limited.
    }

    sendsThisUpdate++;

    if (_queue.empty() && !_cfg.enableAppReliability) {
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
    BinaryPacket::AckSummaryPayload ack = {};

    if (isTimeSyncPacket(packet, sessionMs)) {
      _tdmaClock.applySync(sessionMs);
      Serial.print("[Radio] TIME_SYNC rcv  sessionMs=");
      Serial.println(sessionMs);
      continue;
    }

    if (_cfg.enableAppReliability && isAckSummaryPacket(packet, ack)) {
      Serial.print("[Radio] ACK_SUMMARY rcv  node=");
      Serial.print(ack.node_id);
      Serial.print("  base_seq=");
      Serial.print(ack.ack_base_seq);
      Serial.print("  mask=0x");
      Serial.println(ack.ack_mask, HEX);
      applyAckSummary(ack);
    }
  }
}

bool TdmaRadioService::isTimeSyncPacket(
    const ITdmaRadioDriver::ReceivedPacket &packet,
    uint32_t &sessionMsOut) const {
  BinaryPacket::PktHeader hdr;
  BinaryPacket::TimeSyncPayload ts;

  if (!BinaryPacket::decodeTimeSync(packet.data, packet.len, hdr, ts)) {
    return false;
  }

  sessionMsOut = ts.session_time_ms;
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
      hdr.pkt_type == BinaryPacket::PKT_AWAKEN ||
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
      e.inUse = false;
      if (_pendingCount > 0) {
        _pendingCount--;
      }
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
      e.inUse = false;
      if (_pendingCount > 0) {
        _pendingCount--;
      }
    }
  }
}
