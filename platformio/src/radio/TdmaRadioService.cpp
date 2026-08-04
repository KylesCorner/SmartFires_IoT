// ---
// description: Implements TdmaRadioService's TDMA TX draining, retransmit/ACK-summary handling, time-sync/command RX, and RX power gating.
// role: implementation
// docs: [bandwidth-scaling, packet-reliability, tdma-protocol]
// ---
#include "radio/TdmaRadioService.h"

#include "logging/DebugLogger.h"
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

const char *telemetryModeName(const TdmaConfig &cfg) {
  switch (cfg.reliabilityMode) {
  case TdmaReliabilityMode::StrictLinkAck:
    return "STRICT_LINK_ACK";
  case TdmaReliabilityMode::AppLayerAckSummary:
    return "APP_ACK_SUMMARY";
  }

  return "UNKNOWN";
}

const char *pktTypeName(uint8_t pktType) {
  switch (pktType) {
  case BinaryPacket::PKT_AWAKEN:
    return "AWAKEN";
  case BinaryPacket::PKT_BUNDLE:
    return "BUNDLE";
  case BinaryPacket::PKT_STATUS:
    return "STATUS";
  case BinaryPacket::PKT_FULL_STATE:
    return "FULL_STATE";
  case BinaryPacket::PKT_TIME_SYNC:
    return "TIME_SYNC";
  case BinaryPacket::PKT_ACK_SUMMARY:
    return "ACK_SUMMARY";
  case BinaryPacket::PKT_CMD_CALIBRATE:
    return "CMD_CALIBRATE";
  case BinaryPacket::PKT_CMD_RESET:
    return "CMD_RESET";
  case BinaryPacket::PKT_CALIBRATION_DATA:
    return "CALIBRATION_DATA";
  case BinaryPacket::PKT_CMD_ACK:
    return "CMD_ACK";
  default:
    return "UNKNOWN";
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

// Conservative slot-budget estimates to prevent crossing slot boundaries.
// Values include airtime plus software/radio overhead margin. Named
// constants live in config/NetworkConfig.h (single source, also used by its
// slotWidthMs static_assert) rather than as magic numbers here.
uint16_t estimateTxBudgetMs(const uint8_t *payload, uint8_t len) {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    return NetworkConfig::kDefaultTxBudgetMs;
  }

  BinaryPacket::PktHeader hdr;
  memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));

  if (hdr.magic != BinaryPacket::PKT_MAGIC) {
    return NetworkConfig::kDefaultTxBudgetMs;
  }

  switch (hdr.pkt_type) {
  case BinaryPacket::PKT_BUNDLE:
    return NetworkConfig::kBundleTxBudgetMs;
  case BinaryPacket::PKT_STATUS:
    return NetworkConfig::kStatusTxBudgetMs;
  case BinaryPacket::PKT_AWAKEN:
    return NetworkConfig::kAwakenTxBudgetMs;
  case BinaryPacket::PKT_FULL_STATE:
    return NetworkConfig::kDefaultTxBudgetMs;
  default:
    return NetworkConfig::kDefaultTxBudgetMs;
  }
}

} // namespace

uint32_t TdmaRadioService::computeRetryWaitMs() const {
  const uint32_t scaled =
      (_cfg.expectedAckIntervalMs *
       static_cast<uint32_t>(_cfg.retryWaitMultiplierPermille)) /
      1000u;
  if (scaled < _cfg.retryWaitMinMs) {
    return _cfg.retryWaitMinMs;
  }
  if (scaled > _cfg.retryWaitMaxMs) {
    return _cfg.retryWaitMaxMs;
  }
  return scaled;
}

TdmaRadioService::TdmaRadioService(const TdmaConfig &cfg,
                                   TdmaClock &tdmaClock,
                                   TdmaTxQueue &queue,
                                   ITdmaRadioDriver &driver)
    : _cfg(cfg), _tdmaClock(tdmaClock), _queue(queue), _driver(driver) {}

bool TdmaRadioService::begin() {
  _error = TdmaRadioError::None;

  LOG_INFO("radio",
           "begin node_id=%u base_addr=%u num_slots=%u mode=%s "
           "link_ack=%u app_reliability=%u max_retries=%u ack_timeout_ms=%lu",
           static_cast<unsigned int>(_cfg.nodeId),
           static_cast<unsigned int>(_cfg.baseAddr),
           static_cast<unsigned int>(_cfg.numSlots), telemetryModeName(_cfg),
           _cfg.enableLinkAck ? 1 : 0, _cfg.enableAppReliability ? 1 : 0,
           static_cast<unsigned int>(_cfg.maxRetries),
           static_cast<unsigned long>(_cfg.ackTimeoutMs));

  if (!_driver.begin()) {
    _state = TdmaRadioState::Error;
    _error = TdmaRadioError::BeginFailed;
    LOG_ERROR("radio", "begin_failed");
    return false;
  }

  _state = TdmaRadioState::Ready;
  LOG_INFO("radio", "begin_ok state=ready");

  return true;
}

// void TdmaRadioService::update() {
//   if (_state != TdmaRadioState::Ready) {
//     return;
//   }

//   updateRxPower();
//   drainTxQueue();
//   maybeLogRetransmitHealth();
// }

void TdmaRadioService::update() {
  if (_state != TdmaRadioState::Ready) {
    return;
  }

  if (_dutySleepRequested) {
    if (!_radioAsleep) {
      _driver.sleep();
      _radioAsleep = true;
    }

    return;
  }

  updateRxPower();
  drainTxQueue();
  maybeLogRetransmitHealth();
}

bool TdmaRadioService::sendAwakenHandshake(const uint8_t *payload, uint8_t len) {
  if (_state != TdmaRadioState::Ready) {
    LOG_WARN("radio", "awaken_direct_reject reason=not_ready state=%d",
             static_cast<int>(_state));
    return false;
  }

  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodeHeader(payload, len, hdr);

  if (!hasHdr || hdr.pkt_type != BinaryPacket::PKT_AWAKEN) {
    LOG_WARN("radio",
             "awaken_direct_reject pkt=%s seq=%u len=%u reason=not_awaken_packet",
             hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
             static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
             static_cast<unsigned int>(len));
    return false;
  }

  const bool ok = _driver.sendToWait(payload, len, _cfg.baseAddr);
  _radioAsleep = false;

  if (!ok) {
    _error = TdmaRadioError::SendFailed;
  }

  LOG_INFO("radio",
           "awaken_direct_send pkt=%s seq=%u len=%u link_ack=%s ok=%u",
           pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
           static_cast<unsigned int>(len), ok ? "OK" : "NO", ok ? 1 : 0);

  return ok;
}

bool TdmaRadioService::sendImmediate(const uint8_t *payload,
                                     uint8_t len,
                                     bool requireLinkAck) {
  if (_state != TdmaRadioState::Ready || !payload || len == 0) {
    LOG_WARN("radio",
             "immediate_reject reason=%s state=%d len=%u",
             (_state != TdmaRadioState::Ready)
                 ? "not_ready"
                 : ((!payload || len == 0) ? "invalid_payload" : "unknown"),
             static_cast<int>(_state), static_cast<unsigned int>(len));
    return false;
  }

  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodeHeader(payload, len, hdr);

  LOG_INFO("radio",
           "immediate_tx_start pkt=%s seq=%u len=%u require_link_ack=%u",
           hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
           static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
           static_cast<unsigned int>(len), requireLinkAck ? 1 : 0);

  const bool ok = requireLinkAck
                      ? _driver.sendToWait(payload, len, _cfg.baseAddr)
                      : _driver.send(payload, len, _cfg.baseAddr);
  _radioAsleep = false;

  if (!ok) {
    _error = TdmaRadioError::SendFailed;
    _failedSendCount++;

    LOG_ERROR("radio",
              "immediate_tx_failed count=%lu pkt=%s seq=%u len=%u link_ack=%u",
              static_cast<unsigned long>(_failedSendCount),
              hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
              static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
              static_cast<unsigned int>(len), requireLinkAck ? 1 : 0);

    return false;
  }

  _sentCount++;

  LOG_INFO("radio",
           "immediate_tx_sent count=%lu pkt=%s seq=%u len=%u link_ack=%u",
           static_cast<unsigned long>(_sentCount),
           hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
           static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
           static_cast<unsigned int>(len), requireLinkAck ? 1 : 0);

  return true;
}

bool TdmaRadioService::takePendingCommand(ReceivedCommand &out) {
  if (!_hasPendingCommand) {
    return false;
  }

  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodeHeader(_pendingCommand.data, _pendingCommand.len, hdr);

  out = _pendingCommand;

  LOG_INFO("radio",
           "cmd_take type=%s seq=%u from=%u len=%u rssi=%d",
           hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
           static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
           static_cast<unsigned int>(_pendingCommand.from),
           static_cast<unsigned int>(_pendingCommand.len),
           static_cast<int>(_pendingCommand.rssi));

  _hasPendingCommand = false;
  _pendingCommand = {};
  return true;
}

bool TdmaRadioService::enqueueTelemetry(const uint8_t *payload, uint8_t len) {
  if (_state != TdmaRadioState::Ready) {
    LOG_WARN("radio", "enqueue_reject reason=not_ready state=%d",
             static_cast<int>(_state));
    return false;
  }

  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodeHeader(payload, len, hdr);

  if (hasHdr && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
    LOG_WARN("radio",
             "enqueue_reject pkt=%s seq=%u len=%u reason=awaken_handshake_only",
             pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
             static_cast<unsigned int>(len));
    return false;
  }

  const uint32_t droppedBefore = _queue.droppedOldestCount();

  if (!_queue.enqueue(payload, len)) {
    _error = TdmaRadioError::EnqueueFailed;

    LOG_ERROR("radio",
              "enqueue_failed pkt=%s seq=%u len=%u q=%u/%u pending=%u dropped=%lu",
              hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
              static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
              static_cast<unsigned int>(len),
              static_cast<unsigned int>(_queue.count()),
              static_cast<unsigned int>(_queue.capacity()),
              static_cast<unsigned int>(_pendingCount),
              static_cast<unsigned long>(_queue.droppedOldestCount()));

    return false;
  }

  _enqueuedCount++;

  if (_queue.droppedOldestCount() != droppedBefore) {
    LOG_WARN("radio",
             "drop_oldest drop_count=%lu cause=queue_full incoming=%s seq=%u "
             "q=%u/%u pending=%u dropped=%lu",
             static_cast<unsigned long>(_queue.droppedOldestCount()),
             hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
             static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
             static_cast<unsigned int>(_queue.count()),
             static_cast<unsigned int>(_queue.capacity()),
             static_cast<unsigned int>(_pendingCount),
             static_cast<unsigned long>(_queue.droppedOldestCount()));
  }

  LOG_INFO("radio",
           "enqueue count=%lu pkt=%s seq=%u len=%u q=%u/%u pending=%u dropped=%lu",
           static_cast<unsigned long>(_enqueuedCount),
           hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
           static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
           static_cast<unsigned int>(len), static_cast<unsigned int>(_queue.count()),
           static_cast<unsigned int>(_queue.capacity()),
           static_cast<unsigned int>(_pendingCount),
           static_cast<unsigned long>(_queue.droppedOldestCount()));

  return true;
}

void TdmaRadioService::flushTelemetryBuffers(const char *reason) {
  uint8_t droppedQueued = 0;
  uint8_t throwawayLen = 0;
  uint8_t throwaway[TdmaConfig::MaxPayloadLen] = {};

  while (_queue.dequeue(throwaway, throwawayLen)) {
    droppedQueued = static_cast<uint8_t>(droppedQueued + 1u);
  }

  _queue.clear();

  uint8_t droppedPending = 0;
  for (uint8_t i = 0; i < kMaxReliabilityWindow; ++i) {
    if (_pending[i].inUse) {
      _pending[i] = PendingEntry{};
      droppedPending = static_cast<uint8_t>(droppedPending + 1u);
    }
  }

  _pendingCount = 0;
  _hasFreshTelemetrySent = false;

  LOG_WARN("radio",
           "telemetry_flush reason=%s dropped_queued=%u dropped_pending=%u q=%u/%u pending=%u",
           reason ? reason : "manual", static_cast<unsigned int>(droppedQueued),
           static_cast<unsigned int>(droppedPending),
           static_cast<unsigned int>(_queue.count()),
           static_cast<unsigned int>(_queue.capacity()),
           static_cast<unsigned int>(_pendingCount));
}

uint8_t TdmaRadioService::nodeId() const { return _cfg.nodeId; }

uint8_t TdmaRadioService::numSlots() const { return _cfg.numSlots; }

TdmaRadioState TdmaRadioService::state() const { return _state; }

TdmaRadioError TdmaRadioService::error() const { return _error; }

uint8_t TdmaRadioService::queuedCount() const { return _queue.count(); }

uint32_t TdmaRadioService::sentCount() const { return _sentCount; }

uint32_t TdmaRadioService::retransmitCount() const { return _retransmitCount; }

uint32_t TdmaRadioService::failedSendCount() const { return _failedSendCount; }

uint32_t TdmaRadioService::droppedOldestCount() const {
  return _queue.droppedOldestCount();
}

uint32_t TdmaRadioService::lastTxSlotIndex() const { return _lastTxSlotIndex; }

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
  const uint32_t slotEndMs =
      (_cfg.slotWidthMs > _cfg.guardMs) ? (_cfg.slotWidthMs - _cfg.guardMs)
                                        : _cfg.slotWidthMs;

  const uint8_t kMaxSendsPerUpdate = 3;
  uint8_t sendsThisUpdate = 0;

  while (sendsThisUpdate < kMaxSendsPerUpdate) {
    if (useSlotBudget) {
      const uint32_t posInSlot = _tdmaClock.positionInSlotMs();

      if (posInSlot >= slotEndMs) {
        LOG_DEBUG("radio",
                  "slot_budget_exhausted slot=%lu pos_ms=%lu slot_end_ms=%lu",
                  static_cast<unsigned long>(slotIndex),
                  static_cast<unsigned long>(posInSlot),
                  static_cast<unsigned long>(slotEndMs));
        return;
      }
    } else if (sendsThisUpdate > 0) {
      return;
    }

    uint8_t payload[TdmaConfig::MaxPayloadLen] = {};
    uint8_t len = 0;
    bool fromQueue = false;
    uint8_t pendingIndex = 0;
    uint8_t retrySeq = 0;

    const bool allowRetxThisSlot =
        useAppReliability && (_lastRetxAttemptSlotIndex != slotIndex);

    bool selectedPacket = false;

    if (allowRetxThisSlot &&
        pickRetransmitCandidate(payload, len, retrySeq, pendingIndex)) {
      _lastRetxAttemptSlotIndex = slotIndex;
      selectedPacket = true;
    }

    if (!selectedPacket && !_queue.empty()) {
      if (!_queue.dequeue(payload, len)) {
        LOG_WARN("radio", "dequeue_failed q=%u/%u pending=%u dropped=%lu",
                 static_cast<unsigned int>(_queue.count()),
                 static_cast<unsigned int>(_queue.capacity()),
                 static_cast<unsigned int>(_pendingCount),
                 static_cast<unsigned long>(_queue.droppedOldestCount()));
        return;
      }

      fromQueue = true;
      selectedPacket = true;
    }

    if (!selectedPacket && useAppReliability) {
      if (!pickRetransmitCandidate(payload, len, retrySeq, pendingIndex)) {
        return;
      }
      selectedPacket = true;
    }

    if (!selectedPacket) {
      return;
    }

    if (useSlotBudget) {
      const uint32_t posInSlot = _tdmaClock.positionInSlotMs();
      const uint32_t remainingMs =
          (slotEndMs > posInSlot) ? (slotEndMs - posInSlot) : 0;
      const uint16_t neededMs = estimateTxBudgetMs(payload, len);

      if (remainingMs < neededMs) {
        BinaryPacket::PktHeader deferHdr = {};
        const bool hasDeferHdr = decodeHeader(payload, len, deferHdr);

        LOG_DEBUG("radio",
                  "slot_defer pkt=%s seq=%u len=%u slot=%lu remaining_ms=%lu "
                  "needed_ms=%u from_queue=%u",
                  hasDeferHdr ? pktTypeName(deferHdr.pkt_type) : "RAW",
                  static_cast<unsigned int>(hasDeferHdr ? deferHdr.seq : 0),
                  static_cast<unsigned int>(len),
                  static_cast<unsigned long>(slotIndex),
                  static_cast<unsigned long>(remainingMs),
                  static_cast<unsigned int>(neededMs), fromQueue ? 1 : 0);

        if (fromQueue) {
          const uint32_t droppedBefore = _queue.droppedOldestCount();
          const bool enqOk = _queue.enqueue(payload, len);

          if (!enqOk) {
            _error = TdmaRadioError::EnqueueFailed;
            LOG_ERROR("radio",
                      "slot_defer_requeue_failed pkt=%s seq=%u q=%u/%u "
                      "pending=%u dropped=%lu",
                      hasDeferHdr ? pktTypeName(deferHdr.pkt_type) : "RAW",
                      static_cast<unsigned int>(hasDeferHdr ? deferHdr.seq : 0),
                      static_cast<unsigned int>(_queue.count()),
                      static_cast<unsigned int>(_queue.capacity()),
                      static_cast<unsigned int>(_pendingCount),
                      static_cast<unsigned long>(_queue.droppedOldestCount()));
          } else if (_queue.droppedOldestCount() != droppedBefore) {
            LOG_WARN("radio",
                     "drop_oldest drop_count=%lu cause=slot_defer_requeue "
                     "incoming=%s seq=%u q=%u/%u pending=%u dropped=%lu",
                     static_cast<unsigned long>(_queue.droppedOldestCount()),
                     hasDeferHdr ? pktTypeName(deferHdr.pkt_type) : "RAW",
                     static_cast<unsigned int>(hasDeferHdr ? deferHdr.seq : 0),
                     static_cast<unsigned int>(_queue.count()),
                     static_cast<unsigned int>(_queue.capacity()),
                     static_cast<unsigned int>(_pendingCount),
                     static_cast<unsigned long>(_queue.droppedOldestCount()));
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

    const uint16_t attemptCountWide =
        static_cast<uint16_t>(_cfg.maxRetries) + 1u;
    const uint8_t maxAttempts = attemptCountWide > 0xFFu
                                    ? 0xFFu
                                    : static_cast<uint8_t>(attemptCountWide);

    bool ok = false;
    uint8_t attemptsUsed = 0;

    if (!useLinkAck) {
      LOG_DEBUG("radio", "tx_noack pkt=%s seq=%u len=%u slot=%lu",
                pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
                static_cast<unsigned int>(len),
                static_cast<unsigned long>(slotIndex));

      ok = _driver.send(payload, len, _cfg.baseAddr);
      _radioAsleep = false;
    }

    for (uint8_t attempt = 1; useLinkAck && attempt <= maxAttempts; ++attempt) {
      attemptsUsed = attempt;

      LOG_DEBUG("radio",
                "link_ack_tx pkt=%s seq=%u len=%u slot=%lu attempt=%u/%u "
                "timeout_ms=%lu",
                pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
                static_cast<unsigned int>(len),
                static_cast<unsigned long>(slotIndex),
                static_cast<unsigned int>(attempt),
                static_cast<unsigned int>(maxAttempts),
                static_cast<unsigned long>(_cfg.ackTimeoutMs));

      if (_driver.sendToWait(payload, len, _cfg.baseAddr)) {
        ok = true;
        _radioAsleep = false;

        LOG_DEBUG("radio", "link_ack_rx ack_ok seq=%u attempt=%u retries_used=%u",
                  static_cast<unsigned int>(hdr.seq),
                  static_cast<unsigned int>(attempt),
                  static_cast<unsigned int>(attempt - 1));

        break;
      }

      LOG_WARN("radio",
               "link_ack_rx ack_timeout seq=%u attempt=%u retries_remaining=%u",
               static_cast<unsigned int>(hdr.seq),
               static_cast<unsigned int>(attempt),
               static_cast<unsigned int>(maxAttempts - attempt));
    }

    if (ok) {
      _sentCount++;

      if (useAppReliability) {
        if (fromQueue) {
          rememberSentTelemetry(payload, len, true);
          _lastFreshTelemetrySentMs = _tdmaClock.sessionNowMs();
          _hasFreshTelemetrySent = true;

          LOG_INFO("radio",
                   "tx_sent count=%lu pkt=%s seq=%u len=%u slot=%lu mode=%s "
                   "link_ack=%s retries_used=%u q=%u/%u pending=%u dropped=%lu",
                   static_cast<unsigned long>(_sentCount),
                   pktTypeName(hdr.pkt_type),
                   static_cast<unsigned int>(hdr.seq),
                   static_cast<unsigned int>(len),
                   static_cast<unsigned long>(slotIndex), telemetryModeName(_cfg),
                   useLinkAck ? "OK" : "OFF",
                   static_cast<unsigned int>(
                       useLinkAck && attemptsUsed > 0 ? attemptsUsed - 1 : 0),
                   static_cast<unsigned int>(_queue.count()),
                   static_cast<unsigned int>(_queue.capacity()),
                   static_cast<unsigned int>(_pendingCount),
                   static_cast<unsigned long>(_queue.droppedOldestCount()));
        } else {
          _retransmitCount++;

          LOG_INFO("radio",
                   "retx_sent count=%lu pkt=%s seq=%u len=%u attempt=%u "
                   "slot=%lu mode=%s link_ack=%s retries_used=%u q=%u/%u "
                   "pending=%u dropped=%lu",
                   static_cast<unsigned long>(_retransmitCount),
                   pktTypeName(hdr.pkt_type),
                   static_cast<unsigned int>(hdr.seq),
                   static_cast<unsigned int>(len),
                   static_cast<unsigned int>(_pending[pendingIndex].attempts + 1),
                   static_cast<unsigned long>(slotIndex), telemetryModeName(_cfg),
                   useLinkAck ? "OK" : "OFF",
                   static_cast<unsigned int>(
                       useLinkAck && attemptsUsed > 0 ? attemptsUsed - 1 : 0),
                   static_cast<unsigned int>(_queue.count()),
                   static_cast<unsigned int>(_queue.capacity()),
                   static_cast<unsigned int>(_pendingCount),
                   static_cast<unsigned long>(_queue.droppedOldestCount()));

          markRetransmitSent(pendingIndex);
        }
      } else {
        LOG_INFO("radio",
                 "tx_sent count=%lu pkt=%s seq=%u len=%u slot=%lu q=%u/%u "
                 "pending=%u dropped=%lu",
                 static_cast<unsigned long>(_sentCount), pktTypeName(hdr.pkt_type),
                 static_cast<unsigned int>(hdr.seq),
                 static_cast<unsigned int>(len),
                 static_cast<unsigned long>(slotIndex),
                 static_cast<unsigned int>(_queue.count()),
                 static_cast<unsigned int>(_queue.capacity()),
                 static_cast<unsigned int>(_pendingCount),
                 static_cast<unsigned long>(_queue.droppedOldestCount()));
      }
    } else {
      _failedSendCount++;
      _error = TdmaRadioError::SendFailed;

      LOG_ERROR("radio",
                "tx_failed count=%lu pkt=%s seq=%u len=%u mode=%s link_ack=%s "
                "retries_used=%u from_queue=%u pending_attempt=%u q=%u/%u "
                "pending=%u dropped=%lu",
                static_cast<unsigned long>(_failedSendCount),
                pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
                static_cast<unsigned int>(len), telemetryModeName(_cfg),
                useLinkAck ? "NO" : "OFF",
                static_cast<unsigned int>(
                    useLinkAck
                        ? (attemptsUsed > 0 ? attemptsUsed - 1 : _cfg.maxRetries)
                        : 0),
                fromQueue ? 1 : 0,
                static_cast<unsigned int>(
                    (!fromQueue && useAppReliability &&
                     pendingIndex < kMaxReliabilityWindow &&
                     _pending[pendingIndex].inUse)
                        ? _pending[pendingIndex].attempts + 1
                        : 0),
                static_cast<unsigned int>(_queue.count()),
                static_cast<unsigned int>(_queue.capacity()),
                static_cast<unsigned int>(_pendingCount),
                static_cast<unsigned long>(_queue.droppedOldestCount()));

      // In APP_ACK_SUMMARY mode, preserve first-send failures for bounded local
      // retry so they can still become retransmit candidates.
      if (fromQueue && useAppReliability) {
        rememberSentTelemetry(payload, len, false);

        LOG_WARN("radio",
                 "tx_failed_retained pkt=%s seq=%u len=%u reason=send_failed_local_retry "
                 "q=%u/%u pending=%u dropped=%lu",
                 pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
                 static_cast<unsigned int>(len),
                 static_cast<unsigned int>(_queue.count()),
                 static_cast<unsigned int>(_queue.capacity()),
                 static_cast<unsigned int>(_pendingCount),
                 static_cast<unsigned long>(_queue.droppedOldestCount()));
      } else if (fromQueue) {
        LOG_WARN("radio",
                 "drop_tx_fail drop_count=%lu pkt=%s seq=%u len=%u "
                 "reason=send_failed q=%u/%u pending=%u dropped=%lu",
                 static_cast<unsigned long>(_queue.droppedOldestCount() +
                                            _pendingDropCount + 1),
                 pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
                 static_cast<unsigned int>(len),
                 static_cast<unsigned int>(_queue.count()),
                 static_cast<unsigned int>(_queue.capacity()),
                 static_cast<unsigned int>(_pendingCount),
                 static_cast<unsigned long>(_queue.droppedOldestCount()));
      }
    }

    sendsThisUpdate++;

    if (_queue.empty() && !useAppReliability) {
      return;
    }
  }
}

void TdmaRadioService::updateRxPower() {
  // StrictLinkAck mode needs the radio listening right after its own TX slot
  // for the link-layer ACK (sendToWait(), used by drainTxQueue() in that
  // mode) — gating Rx there would break that wait. It's diagnostics-only and
  // not power-sensitive, so it simply keeps today's always-on Rx behavior.
  const bool wantRx = _cfg.reliabilityMode != TdmaReliabilityMode::AppLayerAckSummary ||
                      _tdmaClock.baseRxWindowOpen();

  if (wantRx) {
    if (_radioAsleep) {
      LOG_DEBUG("radio", "rx_wake slot=%u",
                static_cast<unsigned int>(_tdmaClock.currentSlotNumber()));
      _radioAsleep = false;
    }

    checkIncomingTimeSync();
    return;
  }

  if (!_radioAsleep) {
    _driver.sleep();
    _radioAsleep = true;

    LOG_DEBUG("radio", "rx_sleep slot=%u",
              static_cast<unsigned int>(_tdmaClock.currentSlotNumber()));
  }
}

void TdmaRadioService::checkIncomingTimeSync() {
  while (_driver.available()) {
    ITdmaRadioDriver::ReceivedPacket packet;

    // autoAck=false: letting RadioHead ack automatically routes every
    // unicast receipt (TIME_SYNC-direct, ACK_SUMMARY, CMD_CALIBRATE/RESET)
    // through RHReliableDatagram::acknowledge(), which calls the no-timeout
    // overload of waitPacketSent() — a missed DIO0 TX-done interrupt there
    // hangs the whole node with no recovery (confirmed in the field: see
    // documentation/Current_Architecture/PACKET_RELIABILITY.md). We still
    // want these three packet types acked, since the base blocks on it via
    // sendToWait() — so each branch below calls _driver.acknowledge()
    // itself once it's confirmed the packet is genuinely unicast and worth
    // acking, using the bounded-wait implementation documented on
    // ITdmaRadioDriver::acknowledge() (waits for its own ACK to finish
    // transmitting, capped by NetworkConfig::kAckTxWaitMs, so it can't hang
    // *or* get silently aborted by a sleep() call racing an in-flight send).
    // This matches what SmartFiresBaseApp already does for PKT_AWAKEN on the
    // base side.
    if (!_driver.receive(packet, /*autoAck=*/false)) {
      LOG_WARN("radio", "receive_failed");
      return;
    }

    BinaryPacket::PktHeader hdr = {};
    const bool hasHdr = decodeHeader(packet.data, packet.len, hdr);

    LOG_DEBUG("radio",
              "rx_lora from=%u pkt=%s seq=%u len=%u rssi=%d",
              static_cast<unsigned int>(packet.from),
              hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
              static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
              static_cast<unsigned int>(packet.len), static_cast<int>(packet.rssi));

    uint32_t sessionId = 0;
    uint32_t sessionMs = 0;
    uint8_t assignedNodeId = 0;
    BinaryPacket::AckSummaryPayload ack = {};

    if (isTimeSyncPacket(packet, sessionId, sessionMs, assignedNodeId)) {
      // Only the direct, AWAKEN-triggered TIME_SYNC reply (SmartFiresBaseApp::
      // sendDirectTimeSync(), unicast, sendToWait()) wants an ACK back. The
      // periodic broadcast (RH_BROADCAST_ADDRESS, fire-and-forget) must never
      // get one — every node on the channel would ack the same broadcast at
      // once and collide with each other.
      if (packet.to != ITdmaRadioDriver::kBroadcastAddress) {
        _driver.acknowledge(packet.from, packet.id);
      }

      if (assignedNodeId != 0 && !applyAssignedNodeId(assignedNodeId)) {
        LOG_WARN("radio", "sync_ignore node=%u reason=assignment_apply_failed",
                 static_cast<unsigned int>(assignedNodeId));
        continue;
      }

      _tdmaClock.applySync(sessionId, sessionMs);
      _timeSyncCount++;

      LOG_INFO("tdma",
               "time_sync_received count=%lu session_ms=%lu node=%u assigned=%u",
               static_cast<unsigned long>(_timeSyncCount),
               static_cast<unsigned long>(sessionMs),
               static_cast<unsigned int>(_cfg.nodeId),
               static_cast<unsigned int>(assignedNodeId));

      continue;
    }

    if (_cfg.enableAppReliability && isAckSummaryPacket(packet, ack)) {
      // ACK_SUMMARY is always sent unicast (never broadcast), so no
      // packet.to check is needed here unlike the TIME_SYNC branch above.
      // SmartFiresBaseApp::sendAckSummary() blocks on this via sendToWait().
      _driver.acknowledge(packet.from, packet.id);

      _ackSummaryCount++;

      LOG_INFO("radio",
               "ack_summary_received count=%lu node=%u base_seq=%u mask=0x%04X",
               static_cast<unsigned long>(_ackSummaryCount),
               static_cast<unsigned int>(ack.node_id),
               static_cast<unsigned int>(ack.ack_base_seq),
               static_cast<unsigned int>(ack.ack_mask));

      applyAckSummary(ack);
      continue;
    }

    if (decodeHeader(packet.data, packet.len, hdr) &&
        (hdr.pkt_type == BinaryPacket::PKT_CMD_CALIBRATE ||
         hdr.pkt_type == BinaryPacket::PKT_CMD_RESET)) {
      // Always unicast (never broadcast). SmartFiresBaseApp::
      // sendPendingCommand() blocks on this via sendToWait(); without it,
      // the base gives up after BaseConfig::kMaxPendingCommandSendAttempts
      // and the command is dropped (reason=no_link_ack in its log).
      _driver.acknowledge(packet.from, packet.id);

      rememberPendingCommand(packet);
      continue;
    }

    LOG_DEBUG("radio",
              "rx_unhandled pkt=%s seq=%u from=%u len=%u",
              hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
              static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
              static_cast<unsigned int>(packet.from),
              static_cast<unsigned int>(packet.len));
  }
}

void TdmaRadioService::rememberPendingCommand(
    const ITdmaRadioDriver::ReceivedPacket &packet) {
  const uint8_t clippedLen =
      packet.len > TdmaConfig::MaxPayloadLen ? TdmaConfig::MaxPayloadLen : packet.len;

  memcpy(_pendingCommand.data, packet.data, clippedLen);
  _pendingCommand.len = clippedLen;
  _pendingCommand.rssi = packet.rssi;
  _pendingCommand.from = packet.from;
  _hasPendingCommand = true;

  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodeHeader(packet.data, packet.len, hdr);
  LOG_INFO("radio", "cmd_rx type=%s seq=%u from=%u len=%u rssi=%d",
           hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
           static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
           static_cast<unsigned int>(packet.from),
           static_cast<unsigned int>(packet.len),
           static_cast<int>(packet.rssi));
}

bool TdmaRadioService::isTimeSyncPacket(
    const ITdmaRadioDriver::ReceivedPacket &packet,
    uint32_t &sessionIdOut,
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

  sessionIdOut = ts.session_id;
  sessionMsOut = ts.session_time_ms;
  return true;
}

bool TdmaRadioService::applyAssignedNodeId(uint8_t nodeId) {
  if (nodeId == 0) {
    LOG_WARN("radio", "assignment_reject node=0 reason=invalid");
    return false;
  }

  if (_cfg.nodeId == nodeId) {
    return true;
  }

  const uint8_t oldNodeId = _cfg.nodeId;

  if (!_driver.setLocalAddress(nodeId)) {
    LOG_ERROR("radio", "assignment_failed old_node=%u new_node=%u reason=driver",
              static_cast<unsigned int>(oldNodeId),
              static_cast<unsigned int>(nodeId));
    return false;
  }

  _cfg.nodeId = nodeId;
  _tdmaClock.applyAssignment(nodeId, _cfg.numSlots);

  LOG_INFO("radio", "assignment_applied old_node=%u new_node=%u num_slots=%u",
           static_cast<unsigned int>(oldNodeId),
           static_cast<unsigned int>(_cfg.nodeId),
           static_cast<unsigned int>(_cfg.numSlots));

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

  const bool telemetryType = hdr.pkt_type == BinaryPacket::PKT_BUNDLE ||
                             hdr.pkt_type == BinaryPacket::PKT_STATUS ||
                             hdr.pkt_type == BinaryPacket::PKT_FULL_STATE;

  if (!telemetryType) {
    return false;
  }

  seqOut = hdr.seq;
  return true;
}

void TdmaRadioService::rememberSentTelemetry(const uint8_t *payload,
                                             uint8_t len,
                                             bool sentSuccessfully) {
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
  int8_t replaceIndex = -1;
  uint8_t replaceAttempts = 0;
  uint32_t replaceAgeMs = 0;
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
      if (sentSuccessfully) {
        e.sentSuccessfully = true;
        e.firstSentMs = nowMs;
        e.attempts = 1;
        e.ackGateOpened = false;
      }

      LOG_DEBUG("radio", "pending_refresh seq=%u index=%d len=%u sent_ok=%u",
                static_cast<unsigned int>(seq), static_cast<int>(i),
                static_cast<unsigned int>(len), sentSuccessfully ? 1 : 0);

      return;
    }

    const uint32_t ageMs = nowMs - e.firstSentMs;

    if (replaceIndex < 0 || e.attempts > replaceAttempts ||
        (e.attempts == replaceAttempts && ageMs > replaceAgeMs)) {
      replaceIndex = static_cast<int8_t>(i);
      replaceAttempts = e.attempts;
      replaceAgeMs = ageMs;
      replacedSeq = e.seq;
    }
  }

  int8_t slot = freeIndex;

  if (slot < 0) {
    slot = replaceIndex;
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
  e.sentSuccessfully = sentSuccessfully;
  e.ackGateOpened = false;

  if (replacingPending) {
    _pendingDropCount++;

    LOG_WARN("radio",
             "drop_pending_window drop_count=%lu old_seq=%u new_seq=%u "
             "old_attempts=%u old_age_ms=%lu q=%u/%u pending=%u dropped=%lu",
             static_cast<unsigned long>(_queue.droppedOldestCount() +
                                        _pendingDropCount),
             static_cast<unsigned int>(replacedSeq),
             static_cast<unsigned int>(seq),
             static_cast<unsigned int>(replaceAttempts),
             static_cast<unsigned long>(replaceAgeMs),
             static_cast<unsigned int>(_queue.count()),
             static_cast<unsigned int>(_queue.capacity()),
             static_cast<unsigned int>(_pendingCount),
             static_cast<unsigned long>(_queue.droppedOldestCount()));
  } else {
    LOG_DEBUG("radio", "pending_add seq=%u index=%d len=%u sent_ok=%u pending=%u",
              static_cast<unsigned int>(seq), static_cast<int>(slot),
              static_cast<unsigned int>(len),
              sentSuccessfully ? 1 : 0,
              static_cast<unsigned int>(_pendingCount));
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

  bool hasUnsentPending = false;
  for (uint8_t i = 0; i < windowDepth; ++i) {
    const PendingEntry &e = _pending[i];
    if (e.inUse && !e.sentSuccessfully) {
      hasUnsentPending = true;
      break;
    }
  }

  if (_cfg.reliabilityMode == TdmaReliabilityMode::AppLayerAckSummary &&
      !hasUnsentPending &&
      _cfg.reliabilityFreshTrafficHoldoffMs > 0 && _hasFreshTelemetrySent &&
      (nowMs - _lastFreshTelemetrySentMs) <
          _cfg.reliabilityFreshTrafficHoldoffMs) {
    lenOut = 0;
    return false;
  }

  int8_t bestIndex = -1;
  uint32_t oldestLastSentMs = 0;

  for (uint8_t i = 0; i < windowDepth; ++i) {
    PendingEntry &e = _pending[i];

    if (!e.inUse) {
      continue;
    }

    const uint32_t ageMs = nowMs - e.firstSentMs;

    if (ageMs > _cfg.reliabilityMaxAgeMs ||
        e.attempts >= _cfg.reliabilityMaxAttempts) {
      _pendingDropCount++;

      LOG_WARN("radio",
               "drop_pending drop_count=%lu seq=%u reason=%s attempts=%u "
               "age_ms=%lu q=%u/%u pending=%u dropped=%lu",
               static_cast<unsigned long>(_queue.droppedOldestCount() +
                                          _pendingDropCount),
               static_cast<unsigned int>(e.seq),
               ageMs > _cfg.reliabilityMaxAgeMs ? "max_age" : "max_attempts",
               static_cast<unsigned int>(e.attempts),
               static_cast<unsigned long>(ageMs),
               static_cast<unsigned int>(_queue.count()),
               static_cast<unsigned int>(_queue.capacity()),
               static_cast<unsigned int>(_pendingCount),
               static_cast<unsigned long>(_queue.droppedOldestCount()));

      e.inUse = false;

      if (_pendingCount > 0) {
        _pendingCount--;
      }

      continue;
    }

    if ((nowMs - e.lastSentMs) < _cfg.reliabilityMinRetryGapMs) {
      continue;
    }

    if (_cfg.reliabilityMode == TdmaReliabilityMode::AppLayerAckSummary &&
      e.sentSuccessfully) {
      const uint32_t retryWaitMs = computeRetryWaitMs();

      if (ageMs < retryWaitMs) {
        LOG_DEBUG("radio",
                  "retx_blocked reason=awaiting_ack_window seq=%u age_ms=%lu wait_ms=%lu",
                  static_cast<unsigned int>(e.seq),
                  static_cast<unsigned long>(ageMs),
                  static_cast<unsigned long>(retryWaitMs));
        continue;
      }

      if (!e.ackGateOpened) {
        e.ackGateOpened = true;
        LOG_INFO("radio",
                 "retx_gate_open reason=ack_window_elapsed seq=%u age_ms=%lu wait_ms=%lu",
                 static_cast<unsigned int>(e.seq),
                 static_cast<unsigned long>(ageMs),
                 static_cast<unsigned long>(retryWaitMs));
      }

      if (_cfg.requireAckSummaryBeforeFirstRetry && e.attempts == 1) {
        const bool ackSeen = (_hasReceivedAckSummary &&
                              _lastAckSummarySessionMs > e.firstSentMs);
        if (!ackSeen) {
          if (ageMs >= _cfg.retryWaitMaxMs) {
            LOG_INFO("radio",
                     "retx_gate_open reason=fallback_timeout seq=%u age_ms=%lu",
                     static_cast<unsigned int>(e.seq),
                     static_cast<unsigned long>(ageMs));
            // falls through to candidate selection
          } else {
            LOG_DEBUG("radio",
                      "retx_blocked reason=awaiting_first_ack_summary seq=%u age_ms=%lu",
                      static_cast<unsigned int>(e.seq),
                      static_cast<unsigned long>(ageMs));
            continue;
          }
        }
      }
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

  LOG_DEBUG("radio", "retx_candidate seq=%u index=%d len=%u attempts=%u",
            static_cast<unsigned int>(seqOut), static_cast<int>(bestIndex),
            static_cast<unsigned int>(lenOut),
            static_cast<unsigned int>(e.attempts));

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

  const uint32_t nowMs = _tdmaClock.sessionNowMs();

  if (!e.sentSuccessfully) {
    e.sentSuccessfully = true;
    e.firstSentMs = nowMs;
    e.lastSentMs = nowMs;
    e.attempts = 1;
    e.ackGateOpened = false;

    LOG_INFO("radio",
             "pending_promoted seq=%u index=%u reason=first_successful_send",
             static_cast<unsigned int>(e.seq),
             static_cast<unsigned int>(pendingIndex));
    return;
  }

  e.lastSentMs = nowMs;

  if (e.attempts < 0xFF) {
    e.attempts++;
  }

  LOG_DEBUG("radio", "retx_mark_sent seq=%u index=%u attempts=%u",
            static_cast<unsigned int>(e.seq),
            static_cast<unsigned int>(pendingIndex),
            static_cast<unsigned int>(e.attempts));
}

void TdmaRadioService::applyAckSummary(
    const BinaryPacket::AckSummaryPayload &ack) {
  if (ack.node_id != _cfg.nodeId) {
    LOG_DEBUG("radio", "ack_summary_ignore node=%u local_node=%u",
              static_cast<unsigned int>(ack.node_id),
              static_cast<unsigned int>(_cfg.nodeId));
    return;
  }

  _lastAckSummarySessionMs = _tdmaClock.sessionNowMs();
  _hasReceivedAckSummary = true;

  const uint8_t windowDepth =
      (_cfg.reliabilityWindowDepth > kMaxReliabilityWindow)
          ? kMaxReliabilityWindow
          : _cfg.reliabilityWindowDepth;

  for (uint8_t i = 0; i < windowDepth; ++i) {
    PendingEntry &e = _pending[i];

    if (!e.inUse) {
      continue;
    }

    if (!e.sentSuccessfully) {
      LOG_DEBUG("radio",
                "ack_summary_ignore seq=%u reason=never_sent_successfully",
                static_cast<unsigned int>(e.seq));
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
      LOG_INFO("radio",
               "ack_summary_acked seq=%u attempts=%u base_seq=%u mask=0x%04X "
               "q=%u/%u pending=%u dropped=%lu",
               static_cast<unsigned int>(e.seq),
               static_cast<unsigned int>(e.attempts),
               static_cast<unsigned int>(ack.ack_base_seq),
               static_cast<unsigned int>(ack.ack_mask),
               static_cast<unsigned int>(_queue.count()),
               static_cast<unsigned int>(_queue.capacity()),
               static_cast<unsigned int>(_pendingCount),
               static_cast<unsigned long>(_queue.droppedOldestCount()));

      e.inUse = false;

      if (_pendingCount > 0) {
        _pendingCount--;
      }
    } else {
      const uint32_t ageMs = _tdmaClock.sessionNowMs() - e.firstSentMs;

      LOG_INFO("radio",
               "ack_summary_needs_retx seq=%u next_attempt=%u age_ms=%lu "
               "base_seq=%u mask=0x%04X q=%u/%u pending=%u dropped=%lu",
               static_cast<unsigned int>(e.seq),
               static_cast<unsigned int>(e.attempts + 1),
               static_cast<unsigned long>(ageMs),
               static_cast<unsigned int>(ack.ack_base_seq),
               static_cast<unsigned int>(ack.ack_mask),
               static_cast<unsigned int>(_queue.count()),
               static_cast<unsigned int>(_queue.capacity()),
               static_cast<unsigned int>(_pendingCount),
               static_cast<unsigned long>(_queue.droppedOldestCount()));
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

    if (ageMs > _cfg.reliabilityMaxAgeMs ||
        e.attempts >= _cfg.reliabilityMaxAttempts) {
      _pendingDropCount++;

      LOG_WARN("radio",
               "drop_pending drop_count=%lu seq=%u reason=%s attempts=%u "
               "age_ms=%lu q=%u/%u pending=%u dropped=%lu",
               static_cast<unsigned long>(_queue.droppedOldestCount() +
                                          _pendingDropCount),
               static_cast<unsigned int>(e.seq),
               ageMs > _cfg.reliabilityMaxAgeMs ? "max_age" : "max_attempts",
               static_cast<unsigned int>(e.attempts),
               static_cast<unsigned long>(ageMs),
               static_cast<unsigned int>(_queue.count()),
               static_cast<unsigned int>(_queue.capacity()),
               static_cast<unsigned int>(_pendingCount),
               static_cast<unsigned long>(_queue.droppedOldestCount()));

      e.inUse = false;

      if (_pendingCount > 0) {
        _pendingCount--;
      }
    }
  }
}

void TdmaRadioService::maybeLogRetransmitHealth() {
  if (!_cfg.enableAppReliability) {
    return;
  }

  const uint32_t nowMs = _tdmaClock.sessionNowMs();
  if ((nowMs - _lastRetxHealthLogMs) < 10000u) {
    return;
  }

  const uint8_t windowDepth =
      (_cfg.reliabilityWindowDepth > kMaxReliabilityWindow)
          ? kMaxReliabilityWindow
          : _cfg.reliabilityWindowDepth;

  const uint32_t retryWaitMs = computeRetryWaitMs();

  uint8_t unsentPending = 0;
  uint8_t ackWaitPending = 0;
  uint8_t retryReadyPending = 0;

  for (uint8_t i = 0; i < windowDepth; ++i) {
    const PendingEntry &e = _pending[i];
    if (!e.inUse) {
      continue;
    }

    if (!e.sentSuccessfully) {
      unsentPending++;
      continue;
    }

    const uint32_t ageMs = nowMs - e.firstSentMs;
    const uint32_t sinceLastMs = nowMs - e.lastSentMs;

    if (ageMs > _cfg.reliabilityMaxAgeMs ||
        e.attempts >= _cfg.reliabilityMaxAttempts) {
      continue;
    }

    if (sinceLastMs < _cfg.reliabilityMinRetryGapMs) {
      continue;
    }

    if (_cfg.reliabilityMode == TdmaReliabilityMode::AppLayerAckSummary &&
        ageMs < retryWaitMs) {
      ackWaitPending++;
      continue;
    }

    retryReadyPending++;
  }

  LOG_INFO("radio",
           "retx_health q=%u/%u pending=%u unsent=%u ack_wait=%u retry_ready=%u fresh_holdoff_ms=%lu ack_seen=%u",
           static_cast<unsigned int>(_queue.count()),
           static_cast<unsigned int>(_queue.capacity()),
           static_cast<unsigned int>(_pendingCount),
           static_cast<unsigned int>(unsentPending),
           static_cast<unsigned int>(ackWaitPending),
           static_cast<unsigned int>(retryReadyPending),
           static_cast<unsigned long>(
               _hasFreshTelemetrySent ? (nowMs - _lastFreshTelemetrySentMs) : 0u),
           _hasReceivedAckSummary ? 1 : 0);

  _lastRetxHealthLogMs = nowMs;
}

void TdmaRadioService::setDutySleep(
    bool requested) {
  if (_dutySleepRequested == requested) {
    return;
  }

  _dutySleepRequested = requested;

  LOG_INFO(
      "radio",
      "duty_sleep changed=%u",
      requested ? 1 : 0);

  if (requested) {
    if (!_radioAsleep) {
      _driver.sleep();
      _radioAsleep = true;
    }

    return;
  }

  // The hardware may still physically be asleep, but the next
  // available()/send()/sendToWait() call rearms it. Clearing
  // this flag prevents stale software state after wake.
  _radioAsleep = false;
}
