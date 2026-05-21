#include "app/SmartFiresBaseApp.h"

#include <Arduino.h>

namespace {

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

bool isTelemetryPacketType(uint8_t pktType) {
  return pktType == BinaryPacket::PKT_BUNDLE ||
         pktType == BinaryPacket::PKT_STATUS ||
         pktType == BinaryPacket::PKT_FULL_STATE;
}

uint8_t countBits32(uint32_t value) {
  uint8_t count = 0;
  while (value != 0u) {
    count = static_cast<uint8_t>(count + static_cast<uint8_t>(value & 0x01u));
    value >>= 1;
  }
  return count;
}

}

SmartFiresBaseApp::SmartFiresBaseApp(const Config &cfg,
                                     IClock &clock,
                                     ITdmaRadioDriver &radio,
                                     HardwareSerial &jetsonUart,
                                     Print &debugUart)
    : _cfg(cfg),
      _clock(clock),
      _radio(radio),
      _jetsonUart(jetsonUart),
      _debugUart(debugUart) {}

bool SmartFiresBaseApp::begin() {
  _jetsonUart.begin(_cfg.uartBaud);

  if (!_radio.begin()) {
    _debugUart.println("[BaseApp] Radio begin failed");
    return false;
  }

  _initialized = true;
  _lastHealthLogMs = _clock.millis();
  _lastPeriodicTimeSyncMs = _lastHealthLogMs;
  _lastAckSummaryFlushMs = _lastHealthLogMs;
  _sessionId = 0x53460000UL |
               ((static_cast<uint32_t>(_cfg.baseAddr) & 0xFFu) << 8) |
               (static_cast<uint32_t>(_clock.millis()) & 0xFFu);

  _debugUart.println("[BaseApp] Ready");
  return true;
}

void SmartFiresBaseApp::update() {
  if (!_initialized) {
    return;
  }

  processIncomingLoRa();
  maybeSendPendingAckSummaries();
  processIncomingJetsonUart();
  maybeSendPeriodicTimeSync();
  maybeLogHealth();
}

BinaryPacket::TimeSyncPayload SmartFiresBaseApp::baseLocalTimeSyncPayload() const {
  BinaryPacket::TimeSyncPayload ts = {};
  ts.session_id = _sessionId;
  ts.session_time_ms = _clock.millis();
  return ts;
}

void SmartFiresBaseApp::maybeSendPeriodicTimeSync() {
  const uint32_t now = _clock.millis();
  if (now - _lastPeriodicTimeSyncMs < kPeriodicTimeSyncMs) {
    return;
  }

  const BinaryPacket::TimeSyncPayload ts = currentTimeSyncPayload();
  uint8_t payload[BinaryPacket::kTimeSyncLoRaSize] = {};
  const uint8_t seq = _timeSyncSeq++;
  const uint8_t len =
      BinaryPacket::encodeTimeSyncPayload(0, seq, ts, payload, sizeof(payload));
  if (len == 0) {
    _debugUart.println("[BaseApp] TX TIME_SYNC periodic encode failed");
    _lastPeriodicTimeSyncMs = now;
    return;
  }

  const bool ok = _radio.send(payload, len, _cfg.timeSyncBroadcastAddr);
  if (ok) {
    _timeSyncTxCount++;
  }

  _debugUart.print("[BaseApp] TX TIME_SYNC_PERIODIC seq=");
  _debugUart.print(seq);
  _debugUart.print(" source=");
  _debugUart.print(_hasJetsonTime ? "jetson" : "base_local");
  _debugUart.print(" sessionMs=");
  _debugUart.print(ts.session_time_ms);
  _debugUart.print(" to=");
  _debugUart.print(_cfg.timeSyncBroadcastAddr);
  _debugUart.print(" result=");
  _debugUart.println(ok ? "OK" : "FAIL");

  _lastPeriodicTimeSyncMs = now;
}

void SmartFiresBaseApp::processIncomingLoRa() {
  while (_radio.available()) {
    ITdmaRadioDriver::ReceivedPacket pkt;
    if (!_radio.receive(pkt)) {
      _radioReceiveFailCount++;
      _debugUart.print("[BaseApp] RX_FAIL count=");
      _debugUart.println(_radioReceiveFailCount);
      return;
    }

    _lastRxMs = _clock.millis();

    BinaryPacket::PktHeader hdr = {};
    const bool hasHeader = pkt.len >= sizeof(BinaryPacket::PktHeader);
    const bool validHeader = hasHeader &&
                             (memcpy(&hdr, pkt.data, sizeof(BinaryPacket::PktHeader)),
                              hdr.magic == BinaryPacket::PKT_MAGIC);

    if (!validHeader) {
      _rawRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      _awakenRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_BUNDLE) {
      _bundleRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_STATUS) {
      _statusRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_FULL_STATE) {
      _fullStateRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_CALIBRATION_DATA) {
      _calibrationDataRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_CMD_ACK) {
      _cmdAckRxCount++;
    }

    _debugUart.print("[BaseApp] RX from=");
    _debugUart.print(pkt.from);
    _debugUart.print(" type=");
    _debugUart.print(validHeader ? pktTypeName(hdr.pkt_type) : "RAW");
    _debugUart.print(" seq=");
    _debugUart.print(validHeader ? hdr.seq : 0);
    _debugUart.print(" node=");
    _debugUart.print(validHeader ? hdr.node_id : pkt.from);
    _debugUart.print(" len=");
    _debugUart.print(pkt.len);
    _debugUart.print(" rssi=");
    _debugUart.println(pkt.rssi);

    if (validHeader && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      BinaryPacket::AwakenPayload awaken = {};
      if (!BinaryPacket::decodeAwaken(pkt.data, pkt.len, hdr, awaken)) {
        _debugUart.print("[BaseApp][AWAKEN#");
        _debugUart.print(_awakenRxCount);
        _debugUart.println("] decode_failed");
        continue;
      }

      NodeAssignment *assignment = findOrCreateNodeAssignment(awaken.uid_hash);
      _debugUart.print("[BaseApp][AWAKEN#");
      _debugUart.print(_awakenRxCount);
      _debugUart.print("] uid=0x");
      _debugUart.print(awaken.uid_hash, HEX);
      _debugUart.print(" from=");
      _debugUart.print(pkt.from);
      _debugUart.print(" assigned_node=");
      _debugUart.print(assignment ? assignment->nodeId : 0);
      _debugUart.print(" assigned_slot=");
      _debugUart.print(assignment ? static_cast<uint8_t>(assignment->nodeId - 1u) : 0xFFu);
      _debugUart.print(" seq=");
      _debugUart.print(hdr.seq);
      _debugUart.println(" action=send_local_time_sync_and_forward_to_edge");

      const bool syncOk = assignment &&
                          sendDirectTimeSync(pkt.from, assignment->nodeId,
                                             "awaken", hdr.seq);
      _debugUart.print("[BaseApp][AWAKEN#");
      _debugUart.print(_awakenRxCount);
      _debugUart.print("] local_time_sync_result=");
      _debugUart.println(syncOk ? "OK" : "FAIL");
    } else if (validHeader && hdr.pkt_type == BinaryPacket::PKT_STATUS) {
      BinaryPacket::PktHeader statusHdr = {};
      BinaryPacket::StatusPayload status = {};
      if (!BinaryPacket::decodeStatus(pkt.data, pkt.len, statusHdr, status)) {
        _debugUart.print("[BaseApp][STATUS#");
        _debugUart.print(_statusRxCount);
        _debugUart.println("] decode_failed");
      } else {
        const bool gpsValid =
            (status.flags & BinaryPacket::STATUS_GPS_VALID) != 0u;
        const bool battValid =
            (status.flags & BinaryPacket::STATUS_BATT_VALID) != 0u;

        _debugUart.print("[BaseApp][STATUS#");
        _debugUart.print(_statusRxCount);
        _debugUart.print("] node=");
        _debugUart.print(statusHdr.node_id);
        _debugUart.print(" seq=");
        _debugUart.print(statusHdr.seq);
        _debugUart.print(" gps_valid=");
        _debugUart.print(gpsValid ? 1 : 0);
        _debugUart.print(" batt_valid=");
        _debugUart.print(battValid ? 1 : 0);

        if (gpsValid) {
          _debugUart.print(" lat=");
          _debugUart.print(static_cast<float>(status.lat_e7) / 10000000.0f, 6);
          _debugUart.print(" lon=");
          _debugUart.print(static_cast<float>(status.lon_e7) / 10000000.0f, 6);
        }

        if (battValid) {
          _debugUart.print(" batt_mv=");
          _debugUart.print(status.battery_mv);
          _debugUart.print(" batt_pct=");
          _debugUart.print(status.battery_pct);
        }

        _debugUart.println();
      }
    } else if (validHeader && hdr.pkt_type == BinaryPacket::PKT_CALIBRATION_DATA) {
      _debugUart.print("[BaseApp][CALIB#");
      _debugUart.print(_calibrationDataRxCount);
      _debugUart.print("] node=");
      _debugUart.print(hdr.node_id);
      _debugUart.print(" seq=");
      _debugUart.print(hdr.seq);
      _debugUart.println(" action=forward_to_jetson");
    } else if (validHeader && hdr.pkt_type == BinaryPacket::PKT_CMD_ACK) {
      _debugUart.print("[BaseApp][CMD_ACK#");
      _debugUart.print(_cmdAckRxCount);
      _debugUart.print("] node=");
      _debugUart.print(hdr.node_id);
      _debugUart.print(" seq=");
      _debugUart.print(hdr.seq);
      _debugUart.println(" action=forward_to_jetson");
    } else if (validHeader && isTelemetryPacketType(hdr.pkt_type)) {
      handleTelemetryAckSummary(hdr.node_id, hdr.seq);
    }

    uint8_t frame[2 + 1 + 1 + 255 + 1] = {};
    const size_t outLen =
        BinaryPacket::encodeBaseFrame(pkt.rssi, pkt.data, pkt.len, frame, sizeof(frame));
    if (outLen == 0) {
      continue;
    }

    _jetsonUart.write(frame, outLen);
    _rxForwardCount++;

    if (validHeader && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      _debugUart.print("[BaseApp][AWAKEN#");
      _debugUart.print(_awakenRxCount);
      _debugUart.print("] forwarded bytes=");
      _debugUart.println(outLen);
    }
  }
}

bool SmartFiresBaseApp::sendDirectTimeSync(uint8_t radioAddr,
                                           uint8_t nodeId,
                                           const char *reason,
                                           uint8_t triggerSeq) {
  const BinaryPacket::TimeSyncPayload ts = currentTimeSyncPayload();

  uint8_t payload[BinaryPacket::kTimeSyncLoRaSize] = {};
  const uint8_t seq = _timeSyncSeq++;
  const uint8_t len =
      BinaryPacket::encodeTimeSyncPayload(nodeId, seq, ts, payload, sizeof(payload));
  if (len == 0) {
    _debugUart.println("[BaseApp] TX TIME_SYNC local encode failed");
    return false;
  }

  const bool ok = _radio.sendToWait(payload, len, radioAddr);
  _timeSyncTxCount += ok ? 1u : 0u;
  _debugUart.print("[BaseApp] TX TIME_SYNC_LOCAL seq=");
  _debugUart.print(seq);
  _debugUart.print(" to_radio=");
  _debugUart.print(radioAddr);
  _debugUart.print(" assigned_node=");
  _debugUart.print(nodeId);
  _debugUart.print(" assigned_slot=");
  _debugUart.print(static_cast<uint8_t>(nodeId - 1u));
  _debugUart.print(" sessionId=0x");
  _debugUart.print(ts.session_id, HEX);
  _debugUart.print(" sessionMs=");
  _debugUart.print(ts.session_time_ms);
  _debugUart.print(" trigger=");
  _debugUart.print(reason ? reason : "unknown");
  _debugUart.print(" trigger_seq=");
  _debugUart.print(triggerSeq);
  _debugUart.print(" source=");
  _debugUart.print(_hasJetsonTime ? "jetson" : "base_local");
  _debugUart.print(" link_ack=");
  _debugUart.print(ok ? "OK" : "NO");
  _debugUart.print(" result=");
  _debugUart.println(ok ? "OK" : "FAIL");
  return ok;
}

SmartFiresBaseApp::NodeAssignment *SmartFiresBaseApp::findOrCreateNodeAssignment(
    uint32_t uidHash) {
  NodeAssignment *freeAssignment = nullptr;

  for (uint8_t i = 0; i < kMaxAssignedNodes; ++i) {
    NodeAssignment &assignment = _nodeAssignments[i];
    if (assignment.inUse && assignment.uidHash == uidHash) {
      return &assignment;
    }
    if (!assignment.inUse && !freeAssignment) {
      freeAssignment = &assignment;
    }
  }

  if (!freeAssignment) {
    return nullptr;
  }

  const uint8_t slotIndex =
      static_cast<uint8_t>(freeAssignment - _nodeAssignments);
  freeAssignment->inUse = true;
  freeAssignment->uidHash = uidHash;
  freeAssignment->nodeId = static_cast<uint8_t>(kFirstNodeId + slotIndex);
  return freeAssignment;
}

bool SmartFiresBaseApp::handleTelemetryAckSummary(uint8_t nodeId, uint8_t seq) {
  AckTracker *tracker = findOrCreateAckTracker(nodeId);
  if (!tracker) {
    _debugUart.print("[BaseApp] ACK_SUMMARY skip node=");
    _debugUart.print(nodeId);
    _debugUart.println(" reason=no_tracker_slot");
    return false;
  }

  updateTelemetryReceiptWindow(*tracker, seq);
  recordTelemetrySequence(*tracker, seq);
  tracker->dirty = true;
  tracker->dirtyTriggerSeq = seq;
  return true;
}

SmartFiresBaseApp::AckTracker *SmartFiresBaseApp::findOrCreateAckTracker(
    uint8_t nodeId) {
  AckTracker *freeTracker = nullptr;

  for (uint8_t i = 0; i < kMaxAckTrackedNodes; ++i) {
    AckTracker &tracker = _ackTrackers[i];
    if (tracker.inUse && tracker.nodeId == nodeId) {
      return &tracker;
    }
    if (!tracker.inUse && !freeTracker) {
      freeTracker = &tracker;
    }
  }

  if (!freeTracker) {
    return nullptr;
  }

  freeTracker->inUse = true;
  freeTracker->initialized = false;
  freeTracker->nodeId = nodeId;
  freeTracker->ackBaseSeq = 0;
  freeTracker->ackMask = 0;
  freeTracker->dirty = false;
  freeTracker->dirtyTriggerSeq = 0;
  freeTracker->lastSentInitialized = false;
  freeTracker->lastSentAckBaseSeq = 0;
  freeTracker->lastSentAckMask = 0;
  return freeTracker;
}

void SmartFiresBaseApp::recordTelemetrySequence(AckTracker &tracker, uint8_t seq) {
  if (!tracker.initialized) {
    tracker.ackBaseSeq = seq;
    tracker.ackMask = 0;
    tracker.initialized = true;
    return;
  }

  const uint8_t deltaFromBase = static_cast<uint8_t>(seq - tracker.ackBaseSeq);
  if (deltaFromBase == 0) {
    return;
  }

  if (deltaFromBase < 128u) {
    if (deltaFromBase <= 16u) {
      tracker.ackMask |= static_cast<uint16_t>(1u << (deltaFromBase - 1u));
    } else {
      tracker.ackBaseSeq = seq;
      tracker.ackMask = 0;
      return;
    }
  } else {
    return;
  }

  while ((tracker.ackMask & 0x01u) != 0u) {
    tracker.ackBaseSeq = static_cast<uint8_t>(tracker.ackBaseSeq + 1u);
    tracker.ackMask >>= 1;
  }
}

void SmartFiresBaseApp::updateTelemetryReceiptWindow(AckTracker &tracker,
                                                     uint8_t seq) {
  if (!tracker.receiptWindowInitialized) {
    tracker.receiptWindowInitialized = true;
    tracker.receiptWindowStartSeq = seq;
    tracker.receiptWindowMask = 0x01u;
    return;
  }

  uint8_t delta = static_cast<uint8_t>(seq - tracker.receiptWindowStartSeq);
  if (delta >= 128u) {
    return;
  }

  while (delta >= 20u) {
    const uint8_t receivedCount = countBits32(tracker.receiptWindowMask);
    const uint8_t windowEndSeq =
        static_cast<uint8_t>(tracker.receiptWindowStartSeq + 19u);
    _debugUart.print("[BaseApp][SEQ20] node=");
    _debugUart.print(tracker.nodeId);
    _debugUart.print(" range=");
    _debugUart.print(tracker.receiptWindowStartSeq);
    _debugUart.print("-");
    _debugUart.print(windowEndSeq);
    _debugUart.print(" received=");
    _debugUart.print(receivedCount);
    _debugUart.println("/20");

    tracker.receiptWindowStartSeq =
        static_cast<uint8_t>(tracker.receiptWindowStartSeq + 20u);
    tracker.receiptWindowMask = 0;
    delta = static_cast<uint8_t>(seq - tracker.receiptWindowStartSeq);
  }

  tracker.receiptWindowMask |= (1UL << delta);
}

void SmartFiresBaseApp::maybeSendPendingAckSummaries() {
  uint32_t slotIndex = 0;
  if (!ackSummaryWindowOpen(slotIndex)) {
    return;
  }

  const uint32_t now = _clock.millis();
  if (_cfg.ackSummaryMinIntervalMs > 0 &&
      (now - _lastAckSummaryFlushMs) < _cfg.ackSummaryMinIntervalMs) {
    return;
  }

  for (uint8_t offset = 0; offset < kMaxAckTrackedNodes; ++offset) {
    const uint8_t i = static_cast<uint8_t>((_nextAckTrackerFlushIndex + offset) %
                                           kMaxAckTrackedNodes);
    AckTracker &tracker = _ackTrackers[i];
    if (!tracker.inUse || !tracker.initialized || !tracker.dirty) {
      continue;
    }

    const bool unchangedFromLastSent =
        tracker.lastSentInitialized &&
        tracker.lastSentAckBaseSeq == tracker.ackBaseSeq &&
        tracker.lastSentAckMask == tracker.ackMask;

    if (unchangedFromLastSent) {
      tracker.dirty = false;
      continue;
    }

    const bool ok = sendAckSummary(tracker.nodeId, tracker.ackBaseSeq,
                                   tracker.ackMask, "lora_rx_coalesced",
                                   tracker.dirtyTriggerSeq);
    if (!ok) {
      continue;
    }

    tracker.lastSentInitialized = true;
    tracker.lastSentAckBaseSeq = tracker.ackBaseSeq;
    tracker.lastSentAckMask = tracker.ackMask;
    tracker.dirty = false;
    _lastAckSummaryFlushMs = now;
    _lastAckSummaryFlushSlotIndex = slotIndex;
    _nextAckTrackerFlushIndex = static_cast<uint8_t>((i + 1u) % kMaxAckTrackedNodes);
    return;
  }
}

bool SmartFiresBaseApp::ackSummaryWindowOpen(uint32_t &slotIndexOut) const {
  if (_cfg.tdmaNumSlots == 0 || _cfg.tdmaSlotWidthMs == 0) {
    slotIndexOut = 0;
    return true;
  }

  const BinaryPacket::TimeSyncPayload ts = currentTimeSyncPayload();
  const uint32_t sessionMs = ts.session_time_ms;
  const uint32_t slotIndex = sessionMs / _cfg.tdmaSlotWidthMs;
  const uint32_t posInSlot = sessionMs % _cfg.tdmaSlotWidthMs;
  slotIndexOut = slotIndex;

  if (static_cast<uint8_t>(slotIndex % _cfg.tdmaNumSlots) != 0u) {
    return false;
  }

  if (posInSlot < _cfg.tdmaGuardMs) {
    return false;
  }

  if (_cfg.tdmaSlotWidthMs > _cfg.tdmaGuardMs &&
      posInSlot >= (_cfg.tdmaSlotWidthMs - _cfg.tdmaGuardMs)) {
    return false;
  }

  return true;
}

bool SmartFiresBaseApp::sendAckSummary(uint8_t nodeId, uint8_t ackBaseSeq,
                                       uint16_t ackMask, const char *reason,
                                       uint8_t triggerSeq) {
  BinaryPacket::AckSummaryPayload ack = {};
  ack.node_id = nodeId;
  ack.ack_base_seq = ackBaseSeq;
  ack.ack_mask = ackMask;

  uint8_t payload[BinaryPacket::kAckSummaryLoRaSize] = {};
  const uint8_t seq = _ackSummarySeq++;
  const uint8_t len =
      BinaryPacket::encodeAckSummaryPayload(seq, ack, payload, sizeof(payload));
  if (len == 0) {
    _debugUart.println("[BaseApp] TX ACK_SUMMARY local encode failed");
    return false;
  }

  const bool ok = _radio.sendToWait(payload, len, nodeId);
  _ackTxCount += ok ? 1u : 0u;
  _debugUart.print("[BaseApp] TX ACK_SUMMARY_LOCAL seq=");
  _debugUart.print(seq);
  _debugUart.print(" node=");
  _debugUart.print(nodeId);
  _debugUart.print(" ack_base=");
  _debugUart.print(ackBaseSeq);
  _debugUart.print(" mask=0x");
  _debugUart.print(ackMask, HEX);
  _debugUart.print(" trigger=");
  _debugUart.print(reason ? reason : "unknown");
  _debugUart.print(" trigger_seq=");
  _debugUart.print(triggerSeq);
  _debugUart.print(" link_ack=");
  _debugUart.print(ok ? "OK" : "NO");
  _debugUart.print(" result=");
  _debugUart.println(ok ? "OK" : "FAIL");
  return ok;
}

void SmartFiresBaseApp::updateJetsonTimeSource(
    const BinaryPacket::TimeSyncPayload &ts) {
  _hasJetsonTime = true;
  _jetsonSessionId = ts.session_id;
  _jetsonSessionMsAtUpdate = ts.session_time_ms;
  _localMsAtJetsonUpdate = _clock.millis();
}

BinaryPacket::TimeSyncPayload SmartFiresBaseApp::currentTimeSyncPayload() const {
  BinaryPacket::TimeSyncPayload ts = {};

  if (_hasJetsonTime) {
    const uint32_t elapsedMs = _clock.millis() - _localMsAtJetsonUpdate;
    ts.session_id = _jetsonSessionId;
    ts.session_time_ms = _jetsonSessionMsAtUpdate + elapsedMs;
    return ts;
  }

  ts.session_id = _sessionId;
  ts.session_time_ms = _clock.millis();
  return ts;
}

void SmartFiresBaseApp::processIncomingJetsonUart() {
  uint8_t payload[255] = {};
  uint8_t len = 0;

  while (_jetsonUart.available() > 0) {
    const int raw = _jetsonUart.read();
    if (raw < 0) {
      break;
    }

    if (pushJetsonUartByte(static_cast<uint8_t>(raw), payload, len)) {
      BinaryPacket::PktHeader hdr = {};
      const bool validHeader =
          len >= sizeof(BinaryPacket::PktHeader) &&
          (memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader)),
           hdr.magic == BinaryPacket::PKT_MAGIC);
      if (validHeader) {
        _debugUart.print("[BaseApp] UART_CMD type=");
        _debugUart.print(pktTypeName(hdr.pkt_type));
        _debugUart.print(" seq=");
        _debugUart.print(hdr.seq);
        _debugUart.print(" len=");
        _debugUart.println(len);
      }

      if (handleJetsonCommandPayload(payload, len)) {
        _cmdForwardCount++;
      }
      len = 0;
    }
  }
}

bool SmartFiresBaseApp::handleJetsonCommandPayload(const uint8_t *payload, uint8_t len) {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    return false;
  }

  BinaryPacket::PktHeader hdr;
  memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));
  if (hdr.magic != BinaryPacket::PKT_MAGIC) {
    return false;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_TIME_SYNC) {
    BinaryPacket::TimeSyncPayload ts = {};
    BinaryPacket::PktHeader ignored = {};
    if (!BinaryPacket::decodeTimeSync(payload, len, ignored, ts)) {
      return false;
    }

    updateJetsonTimeSource(ts);

    _debugUart.print("[BaseApp] RX TIME_SYNC_UART seq=");
    _debugUart.print(hdr.seq);
    _debugUart.print(" source=jetson");
    _debugUart.print(" sessionMs=");
    _debugUart.print(ts.session_time_ms);
    _debugUart.println(" action=cache_only_not_forwarded");
    return true;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_ACK_SUMMARY) {
    BinaryPacket::PktHeader ignored;
    BinaryPacket::AckSummaryPayload ack;
    if (!BinaryPacket::decodeAckSummary(payload, len, ignored, ack)) {
      return false;
    }
    const bool ok = _radio.send(payload, len, ack.node_id);
    _ackTxCount += ok ? 1u : 0u;
    _debugUart.print("[BaseApp] TX ACK_SUMMARY seq=");
    _debugUart.print(hdr.seq);
    _debugUart.print(" node=");
    _debugUart.print(ack.node_id);
    _debugUart.print(" base_seq=");
    _debugUart.print(ack.ack_base_seq);
    _debugUart.print(" mask=0x");
    _debugUart.print(ack.ack_mask, HEX);
    _debugUart.print(" result=");
    _debugUart.println(ok ? "OK" : "FAIL");
    return ok;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_CMD_CALIBRATE) {
    BinaryPacket::PktHeader ignored;
    BinaryPacket::CmdCalibratePayload cmd = {};
    if (!BinaryPacket::decodeCmdCalibrate(payload, len, ignored, cmd)) {
      return false;
    }

    const bool ok = _radio.sendToWait(payload, len, cmd.node_id);
    _debugUart.print("[BaseApp] TX CMD_CALIBRATE seq=");
    _debugUart.print(hdr.seq);
    _debugUart.print(" node=");
    _debugUart.print(cmd.node_id);
    _debugUart.print(" duration_s=");
    _debugUart.print(cmd.duration_s);
    _debugUart.print(" result=");
    _debugUart.println(ok ? "OK" : "FAIL");
    return ok;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_CMD_RESET) {
    BinaryPacket::PktHeader ignored;
    BinaryPacket::CmdResetPayload cmd = {};
    if (!BinaryPacket::decodeCmdReset(payload, len, ignored, cmd)) {
      return false;
    }

    const bool ok = _radio.sendToWait(payload, len, cmd.node_id);
    _debugUart.print("[BaseApp] TX CMD_RESET seq=");
    _debugUart.print(hdr.seq);
    _debugUart.print(" node=");
    _debugUart.print(cmd.node_id);
    _debugUart.print(" reset_type=");
    _debugUart.print(cmd.reset_type);
    _debugUart.print(" result=");
    _debugUart.println(ok ? "OK" : "FAIL");
    return ok;
  }

  return false;
}

bool SmartFiresBaseApp::pushJetsonUartByte(uint8_t b,
                                           uint8_t *payloadOut,
                                           uint8_t &lenOut) {
  lenOut = 0;

  switch (_uartRx.stage) {
    case UartRxState::Stage::WaitM0:
      if (b == BinaryPacket::FRAME_M0) {
        _uartRx.stage = UartRxState::Stage::WaitM1;
      }
      return false;

    case UartRxState::Stage::WaitM1:
      _uartRx.stage = (b == BinaryPacket::FRAME_M1)
                          ? UartRxState::Stage::WaitLen
                          : UartRxState::Stage::WaitM0;
      return false;

    case UartRxState::Stage::WaitLen:
      if (b == 0) {
        _uartFrameErrorCount++;
        resetJetsonUartRx();
        return false;
      }
      _uartRx.len = b;
      _uartRx.dataPos = 0;
      _uartRx.stage = UartRxState::Stage::ReadData;
      return false;

    case UartRxState::Stage::ReadData:
      _uartRx.data[_uartRx.dataPos++] = b;
      if (_uartRx.dataPos >= _uartRx.len) {
        _uartRx.stage = UartRxState::Stage::WaitCrc;
      }
      return false;

    case UartRxState::Stage::WaitCrc: {
      _uartRx.crc = b;

      uint8_t crcInput[1 + sizeof(_uartRx.data)] = {};
      crcInput[0] = _uartRx.len;
      memcpy(crcInput + 1, _uartRx.data, _uartRx.len);
      const uint8_t expected = BinaryPacket::crc8(crcInput, static_cast<size_t>(_uartRx.len + 1));

      if (_uartRx.crc != expected) {
        _uartFrameErrorCount++;
        resetJetsonUartRx();
        return false;
      }

      if (payloadOut) {
        memcpy(payloadOut, _uartRx.data, _uartRx.len);
      }
      lenOut = _uartRx.len;
      resetJetsonUartRx();
      return true;
    }
  }

  resetJetsonUartRx();
  return false;
}

void SmartFiresBaseApp::resetJetsonUartRx() {
  _uartRx.stage = UartRxState::Stage::WaitM0;
  _uartRx.len = 0;
  _uartRx.dataPos = 0;
  _uartRx.crc = 0;
}

void SmartFiresBaseApp::maybeLogHealth() {
  const uint32_t now = _clock.millis();
  if (now - _lastHealthLogMs < kHealthLogPeriodMs) {
    return;
  }

  const uint32_t lastRxAgoMs = (_lastRxMs == 0) ? 0xFFFFFFFFu : (now - _lastRxMs);

  _debugUart.print("[BaseApp] rx_fwd=");
  _debugUart.print(_rxForwardCount);
  _debugUart.print(" cmd_fwd=");
  _debugUart.print(_cmdForwardCount);
  _debugUart.print(" awaken_rx=");
  _debugUart.print(_awakenRxCount);
  _debugUart.print(" bundle_rx=");
  _debugUart.print(_bundleRxCount);
  _debugUart.print(" status_rx=");
  _debugUart.print(_statusRxCount);
  _debugUart.print(" full_rx=");
  _debugUart.print(_fullStateRxCount);
  _debugUart.print(" calib_rx=");
  _debugUart.print(_calibrationDataRxCount);
  _debugUart.print(" cmd_ack_rx=");
  _debugUart.print(_cmdAckRxCount);
  _debugUart.print(" raw_rx=");
  _debugUart.print(_rawRxCount);
  _debugUart.print(" sync_tx=");
  _debugUart.print(_timeSyncTxCount);
  _debugUart.print(" ack_tx=");
  _debugUart.print(_ackTxCount);
  _debugUart.print(" time_src=");
  _debugUart.print(_hasJetsonTime ? "jetson" : "base_local");
  _debugUart.print(" jetson_sync_age_ms=");
  if (_hasJetsonTime) {
    _debugUart.print(now - _localMsAtJetsonUpdate);
  } else {
    _debugUart.print("n/a");
  }
  _debugUart.print(" rx_fail=");
  _debugUart.print(_radioReceiveFailCount);
  _debugUart.print(" last_rx_ms_ago=");
  if (lastRxAgoMs == 0xFFFFFFFFu) {
    _debugUart.print("never");
  } else {
    _debugUart.print(lastRxAgoMs);
  }
  _debugUart.print(" uart_err=");
  _debugUart.println(_uartFrameErrorCount);

  _lastHealthLogMs = now;
}
