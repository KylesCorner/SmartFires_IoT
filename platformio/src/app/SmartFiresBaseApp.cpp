#include "app/SmartFiresBaseApp.h"

#include "calibration/CalibrationDebug.h"
#include "logging/DebugLogger.h"

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

bool decodeCmdCalibrateFromJetson(const uint8_t *payload,
                                  uint8_t len,
                                  BinaryPacket::PktHeader &hdrOut,
                                  BinaryPacket::CmdCalibratePayload &cmdOut,
                                  bool &legacyNoCrcOut) {
  legacyNoCrcOut = false;

  if (!payload || len < sizeof(BinaryPacket::PktHeader) +
                           sizeof(BinaryPacket::CmdCalibratePayload)) {
    return false;
  }

  if (BinaryPacket::decodeCmdCalibrate(payload, len, hdrOut, cmdOut)) {
    return true;
  }

  if (len == sizeof(BinaryPacket::PktHeader) +
                 sizeof(BinaryPacket::CmdCalibratePayload)) {
    BinaryPacket::PktHeader hdr = {};
    BinaryPacket::CmdCalibratePayload cmd = {};
    memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));
    memcpy(&cmd, payload + sizeof(BinaryPacket::PktHeader),
           sizeof(BinaryPacket::CmdCalibratePayload));

    if (hdr.magic == BinaryPacket::PKT_MAGIC &&
        hdr.pkt_type == BinaryPacket::PKT_CMD_CALIBRATE) {
      hdrOut = hdr;
      cmdOut = cmd;
      legacyNoCrcOut = true;
      return true;
    }
  }

  return false;
}

bool decodeCmdResetFromJetson(const uint8_t *payload,
                              uint8_t len,
                              BinaryPacket::PktHeader &hdrOut,
                              BinaryPacket::CmdResetPayload &cmdOut,
                              bool &legacyNoCrcOut) {
  legacyNoCrcOut = false;

  if (!payload || len < sizeof(BinaryPacket::PktHeader) +
                           sizeof(BinaryPacket::CmdResetPayload)) {
    return false;
  }

  if (BinaryPacket::decodeCmdReset(payload, len, hdrOut, cmdOut)) {
    return true;
  }

  if (len == sizeof(BinaryPacket::PktHeader) + sizeof(BinaryPacket::CmdResetPayload)) {
    BinaryPacket::PktHeader hdr = {};
    BinaryPacket::CmdResetPayload cmd = {};
    memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));
    memcpy(&cmd, payload + sizeof(BinaryPacket::PktHeader),
           sizeof(BinaryPacket::CmdResetPayload));

    if (hdr.magic == BinaryPacket::PKT_MAGIC &&
        hdr.pkt_type == BinaryPacket::PKT_CMD_RESET) {
      hdrOut = hdr;
      cmdOut = cmd;
      legacyNoCrcOut = true;
      return true;
    }
  }

  return false;
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
    LOG_ERROR("base", "radio_begin_failed");
    return false;
  }

  _initialized = true;
  _lastHealthLogMs = _clock.millis();
  _lastPeriodicTimeSyncMs = _lastHealthLogMs;
  _lastAckSummaryFlushMs = _lastHealthLogMs;
  _sessionId = 0x53460000UL |
               ((static_cast<uint32_t>(_cfg.baseAddr) & 0xFFu) << 8) |
               (static_cast<uint32_t>(_clock.millis()) & 0xFFu);

  LOG_INFO("base", "ready base_addr=%u uart_baud=%lu",
           static_cast<unsigned int>(_cfg.baseAddr),
           static_cast<unsigned long>(_cfg.uartBaud));
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
    LOG_ERROR("base", "tx_time_sync_periodic_encode_failed seq=%u",
              static_cast<unsigned int>(seq));
    _lastPeriodicTimeSyncMs = now;
    return;
  }

  const bool ok = _radio.send(payload, len, _cfg.timeSyncBroadcastAddr);
  if (ok) {
    _timeSyncTxCount++;
  }

  LOG_INFO("base",
           "tx_time_sync_periodic seq=%u source=%s session_ms=%lu to=%u result=%s",
           static_cast<unsigned int>(seq), _hasJetsonTime ? "jetson" : "base_local",
           static_cast<unsigned long>(ts.session_time_ms),
           static_cast<unsigned int>(_cfg.timeSyncBroadcastAddr),
           ok ? "OK" : "FAIL");

  _lastPeriodicTimeSyncMs = now;
}

void SmartFiresBaseApp::processIncomingLoRa() {
  while (_radio.available()) {
    ITdmaRadioDriver::ReceivedPacket pkt;
    if (!_radio.receive(pkt)) {
      _radioReceiveFailCount++;
      LOG_WARN("base", "rx_fail count=%lu",
               static_cast<unsigned long>(_radioReceiveFailCount));
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

    LOG_INFO("base", "rx_lora from=%u type=%s seq=%u node=%u len=%u rssi=%d",
         static_cast<unsigned int>(pkt.from),
         validHeader ? pktTypeName(hdr.pkt_type) : "RAW",
         static_cast<unsigned int>(validHeader ? hdr.seq : 0),
         static_cast<unsigned int>(validHeader ? hdr.node_id : pkt.from),
         static_cast<unsigned int>(pkt.len), static_cast<int>(pkt.rssi));

    if (validHeader && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      BinaryPacket::AwakenPayload awaken = {};
      if (!BinaryPacket::decodeAwaken(pkt.data, pkt.len, hdr, awaken)) {
        LOG_WARN("base", "awaken_decode_failed count=%lu from=%u len=%u",
                 static_cast<unsigned long>(_awakenRxCount),
                 static_cast<unsigned int>(pkt.from),
                 static_cast<unsigned int>(pkt.len));
        continue;
      }

      NodeAssignment *assignment = findOrCreateNodeAssignment(awaken.uid_hash);
      LOG_INFO("base",
               "awaken_rx count=%lu uid_hash=0x%08lX from=%u assigned_node=%u assigned_slot=%u seq=%u action=send_local_time_sync_and_forward_to_edge",
               static_cast<unsigned long>(_awakenRxCount),
               static_cast<unsigned long>(awaken.uid_hash),
               static_cast<unsigned int>(pkt.from),
               static_cast<unsigned int>(assignment ? assignment->nodeId : 0),
               static_cast<unsigned int>(
                   assignment ? static_cast<uint8_t>(assignment->nodeId - 1u)
                              : 0xFFu),
               static_cast<unsigned int>(hdr.seq));

      const bool syncOk = assignment &&
                          sendDirectTimeSync(pkt.from, assignment->nodeId,
                                             "awaken", hdr.seq);
      LOG_INFO("base", "awaken_local_time_sync_result count=%lu result=%s",
               static_cast<unsigned long>(_awakenRxCount),
               syncOk ? "OK" : "FAIL");
    } else if (validHeader && hdr.pkt_type == BinaryPacket::PKT_STATUS) {
      BinaryPacket::PktHeader statusHdr = {};
      BinaryPacket::StatusPayload status = {};
      if (!BinaryPacket::decodeStatus(pkt.data, pkt.len, statusHdr, status)) {
        LOG_WARN("base", "status_decode_failed count=%lu node=%u seq=%u len=%u",
                 static_cast<unsigned long>(_statusRxCount),
                 static_cast<unsigned int>(hdr.node_id),
                 static_cast<unsigned int>(hdr.seq),
                 static_cast<unsigned int>(pkt.len));
      } else {
        const bool gpsValid =
            (status.flags & BinaryPacket::STATUS_GPS_VALID) != 0u;
        const bool battValid =
            (status.flags & BinaryPacket::STATUS_BATT_VALID) != 0u;
        LOG_INFO("base",
                 "status_rx count=%lu node=%u seq=%u gps_valid=%u batt_valid=%u lat=%.6f lon=%.6f batt_mv=%u batt_pct=%u",
                 static_cast<unsigned long>(_statusRxCount),
                 static_cast<unsigned int>(statusHdr.node_id),
                 static_cast<unsigned int>(statusHdr.seq), gpsValid ? 1 : 0,
                 battValid ? 1 : 0,
                 gpsValid
                     ? static_cast<double>(static_cast<float>(status.lat_e7) /
                                           10000000.0f)
                     : 0.0,
                 gpsValid
                     ? static_cast<double>(static_cast<float>(status.lon_e7) /
                                           10000000.0f)
                     : 0.0,
                 static_cast<unsigned int>(battValid ? status.battery_mv : 0),
                 static_cast<unsigned int>(battValid ? status.battery_pct : 0));
      }
    } else if (validHeader && hdr.pkt_type == BinaryPacket::PKT_CALIBRATION_DATA) {
      BinaryPacket::PktHeader calibHdr = {};
      BinaryPacket::CalibrationDataPayload calib = {};

      if (BinaryPacket::decodeCalibrationData(pkt.data, pkt.len, calibHdr, calib)) {
        CalibrationDebug::logCalibrationDataSummary(calib, calibHdr.node_id,
                                                    calibHdr.seq, "calib");
      } else {
        LOG_WARN("calib",
                 "calibration_data_decode_failed node=%u seq=%u len=%u",
                 static_cast<unsigned int>(hdr.node_id),
                 static_cast<unsigned int>(hdr.seq),
                 static_cast<unsigned int>(pkt.len));
      }

      LOG_INFO("base",
               "calibration_data_rx count=%lu node=%u seq=%u action=forward_to_jetson",
               static_cast<unsigned long>(_calibrationDataRxCount),
               static_cast<unsigned int>(hdr.node_id),
               static_cast<unsigned int>(hdr.seq));
    } else if (validHeader && hdr.pkt_type == BinaryPacket::PKT_CMD_ACK) {
      BinaryPacket::PktHeader ackHdr = {};
      BinaryPacket::CmdAckPayload ack = {};

      if (BinaryPacket::decodeCmdAck(pkt.data, pkt.len, ackHdr, ack)) {
        CalibrationDebug::logCmdAckSummary(ack, ackHdr.node_id, ackHdr.seq,
                                           "calib");
      } else {
        LOG_WARN("calib", "cmd_ack_decode_failed node=%u seq=%u len=%u",
                 static_cast<unsigned int>(hdr.node_id),
                 static_cast<unsigned int>(hdr.seq),
                 static_cast<unsigned int>(pkt.len));
      }

      LOG_INFO("base", "cmd_ack_rx count=%lu node=%u seq=%u action=forward_to_jetson",
               static_cast<unsigned long>(_cmdAckRxCount),
               static_cast<unsigned int>(hdr.node_id),
               static_cast<unsigned int>(hdr.seq));
    } else if (validHeader && isTelemetryPacketType(hdr.pkt_type)) {
      const bool ackTracked = handleTelemetryAckSummary(hdr.node_id, hdr.seq);
      LOG_INFO("base", "ack_track node=%u seq=%u pkt=%s tracked=%u",
               static_cast<unsigned int>(hdr.node_id),
               static_cast<unsigned int>(hdr.seq), pktTypeName(hdr.pkt_type),
               ackTracked ? 1 : 0);
    }

    uint8_t frame[2 + 1 + 1 + 255 + 1] = {};
    const size_t outLen =
        BinaryPacket::encodeBaseFrame(pkt.rssi, pkt.data, pkt.len, frame, sizeof(frame));
    if (outLen == 0) {
      continue;
    }

    const size_t written = _jetsonUart.write(frame, outLen);
    _rxForwardCount++;

    LOG_INFO("base",
             "rx_fwd to=jetson type=%s seq=%u node=%u bytes=%u written=%u result=%s",
             validHeader ? pktTypeName(hdr.pkt_type) : "RAW",
             static_cast<unsigned int>(validHeader ? hdr.seq : 0),
             static_cast<unsigned int>(validHeader ? hdr.node_id : pkt.from),
             static_cast<unsigned int>(outLen),
             static_cast<unsigned int>(written),
             written == outLen ? "OK" : "PARTIAL");

    if (validHeader && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      LOG_DEBUG("base", "awaken_forwarded count=%lu bytes=%u",
                static_cast<unsigned long>(_awakenRxCount),
                static_cast<unsigned int>(outLen));
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
    LOG_ERROR("base", "tx_time_sync_local_encode_failed seq=%u node=%u",
              static_cast<unsigned int>(seq), static_cast<unsigned int>(nodeId));
    return false;
  }

  const bool ok = _radio.sendToWait(payload, len, radioAddr);
  _timeSyncTxCount += ok ? 1u : 0u;
  LOG_INFO(
      "base",
      "tx_time_sync_local seq=%u to_radio=%u assigned_node=%u assigned_slot=%u session_id=0x%08lX session_ms=%lu trigger=%s trigger_seq=%u source=%s link_ack=%s result=%s",
      static_cast<unsigned int>(seq), static_cast<unsigned int>(radioAddr),
      static_cast<unsigned int>(nodeId),
      static_cast<unsigned int>(static_cast<uint8_t>(nodeId - 1u)),
      static_cast<unsigned long>(ts.session_id),
      static_cast<unsigned long>(ts.session_time_ms),
      reason ? reason : "unknown", static_cast<unsigned int>(triggerSeq),
      _hasJetsonTime ? "jetson" : "base_local", ok ? "OK" : "NO",
      ok ? "OK" : "FAIL");
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
    LOG_WARN("base", "ack_summary_skip node=%u reason=no_tracker_slot",
             static_cast<unsigned int>(nodeId));
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
    LOG_INFO("base", "seq20 node=%u range=%u-%u received=%u/20",
         static_cast<unsigned int>(tracker.nodeId),
         static_cast<unsigned int>(tracker.receiptWindowStartSeq),
         static_cast<unsigned int>(windowEndSeq),
         static_cast<unsigned int>(receivedCount));

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
    LOG_ERROR("base", "tx_ack_summary_local_encode_failed seq=%u node=%u",
              static_cast<unsigned int>(seq), static_cast<unsigned int>(nodeId));
    return false;
  }

  const bool ok = _radio.sendToWait(payload, len, nodeId);
  _ackTxCount += ok ? 1u : 0u;
  LOG_INFO(
      "base",
      "tx_ack_summary_local seq=%u node=%u ack_base=%u mask=0x%04X trigger=%s trigger_seq=%u link_ack=%s result=%s",
      static_cast<unsigned int>(seq), static_cast<unsigned int>(nodeId),
      static_cast<unsigned int>(ackBaseSeq), static_cast<unsigned int>(ackMask),
      reason ? reason : "unknown", static_cast<unsigned int>(triggerSeq),
      ok ? "OK" : "NO", ok ? "OK" : "FAIL");
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
      LOG_DEBUG("base", "uart_frame_ok len=%u", static_cast<unsigned int>(len));

      BinaryPacket::PktHeader hdr = {};
      const bool validHeader =
          len >= sizeof(BinaryPacket::PktHeader) &&
          (memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader)),
           hdr.magic == BinaryPacket::PKT_MAGIC);
      if (validHeader) {
        LOG_INFO("base", "uart_cmd_rx type=%s seq=%u node=%u len=%u",
                 pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.seq),
                 static_cast<unsigned int>(hdr.node_id),
                 static_cast<unsigned int>(len));
      } else {
        LOG_WARN("base", "uart_cmd_invalid_header len=%u",
                 static_cast<unsigned int>(len));
      }

      if (handleJetsonCommandPayload(payload, len)) {
        _cmdForwardCount++;
      } else {
        LOG_WARN("base", "uart_cmd_dropped len=%u type=%s seq=%u node=%u",
                 static_cast<unsigned int>(len),
                 validHeader ? pktTypeName(hdr.pkt_type) : "RAW",
                 static_cast<unsigned int>(validHeader ? hdr.seq : 0),
                 static_cast<unsigned int>(validHeader ? hdr.node_id : 0));
      }
      len = 0;
    }
  }
}

bool SmartFiresBaseApp::handleJetsonCommandPayload(const uint8_t *payload, uint8_t len) {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    LOG_WARN("base", "uart_cmd_reject reason=short_frame len=%u",
             static_cast<unsigned int>(len));
    return false;
  }

  BinaryPacket::PktHeader hdr;
  memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));
  if (hdr.magic != BinaryPacket::PKT_MAGIC) {
    LOG_WARN("base", "uart_cmd_reject reason=bad_magic len=%u",
             static_cast<unsigned int>(len));
    return false;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_TIME_SYNC) {
    BinaryPacket::TimeSyncPayload ts = {};
    BinaryPacket::PktHeader ignored = {};
    if (!BinaryPacket::decodeTimeSync(payload, len, ignored, ts)) {
      LOG_WARN("base", "uart_cmd_reject type=TIME_SYNC reason=decode_failed len=%u",
               static_cast<unsigned int>(len));
      return false;
    }

    updateJetsonTimeSource(ts);

    LOG_INFO("base",
             "rx_time_sync_uart seq=%u source=jetson session_ms=%lu action=cache_only_not_forwarded",
             static_cast<unsigned int>(hdr.seq),
             static_cast<unsigned long>(ts.session_time_ms));
    return true;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_ACK_SUMMARY) {
    BinaryPacket::PktHeader ignored;
    BinaryPacket::AckSummaryPayload ack;
    if (!BinaryPacket::decodeAckSummary(payload, len, ignored, ack)) {
      LOG_WARN("base", "uart_cmd_reject type=ACK_SUMMARY reason=decode_failed len=%u",
               static_cast<unsigned int>(len));
      return false;
    }
    const bool ok = _radio.send(payload, len, ack.node_id);
    _ackTxCount += ok ? 1u : 0u;
    LOG_INFO("base",
             "tx_ack_summary seq=%u node=%u base_seq=%u mask=0x%04X result=%s",
             static_cast<unsigned int>(hdr.seq),
             static_cast<unsigned int>(ack.node_id),
             static_cast<unsigned int>(ack.ack_base_seq),
             static_cast<unsigned int>(ack.ack_mask), ok ? "OK" : "FAIL");
    return ok;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_CMD_CALIBRATE) {
    BinaryPacket::PktHeader cmdHdr = {};
    BinaryPacket::CmdCalibratePayload cmd = {};
    bool legacyNoCrc = false;

    if (!decodeCmdCalibrateFromJetson(payload, len, cmdHdr, cmd, legacyNoCrc)) {
      LOG_WARN("base", "uart_cmd_reject type=CMD_CALIBRATE reason=decode_failed len=%u",
               static_cast<unsigned int>(len));
      return false;
    }

    uint8_t loraPayload[BinaryPacket::kCmdCalibrateLoRaSize] = {};
    const uint8_t loraLen = BinaryPacket::encodeCmdCalibratePayload(
        cmdHdr.seq, cmd, loraPayload, sizeof(loraPayload));
    if (loraLen == 0) {
      LOG_ERROR("base", "tx_cmd_calibrate_encode_failed seq=%u node=%u",
                static_cast<unsigned int>(cmdHdr.seq),
                static_cast<unsigned int>(cmd.node_id));
      return false;
    }

    const bool ok = _radio.sendToWait(loraPayload, loraLen, cmd.node_id);
    LOG_INFO("base", "tx_cmd_calibrate seq=%u node=%u duration_s=%u uart_format=%s lora_len=%u result=%s",
             static_cast<unsigned int>(cmdHdr.seq),
             static_cast<unsigned int>(cmd.node_id),
             static_cast<unsigned int>(cmd.duration_s),
             legacyNoCrc ? "legacy_no_crc" : "lora_crc",
             static_cast<unsigned int>(loraLen), ok ? "OK" : "FAIL");
    return ok;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_CMD_RESET) {
    BinaryPacket::PktHeader cmdHdr = {};
    BinaryPacket::CmdResetPayload cmd = {};
    bool legacyNoCrc = false;

    if (!decodeCmdResetFromJetson(payload, len, cmdHdr, cmd, legacyNoCrc)) {
      LOG_WARN("base", "uart_cmd_reject type=CMD_RESET reason=decode_failed len=%u",
               static_cast<unsigned int>(len));
      return false;
    }

    uint8_t loraPayload[BinaryPacket::kCmdResetLoRaSize] = {};
    const uint8_t loraLen = BinaryPacket::encodeCmdResetPayload(
        cmdHdr.seq, cmd, loraPayload, sizeof(loraPayload));
    if (loraLen == 0) {
      LOG_ERROR("base", "tx_cmd_reset_encode_failed seq=%u node=%u",
                static_cast<unsigned int>(cmdHdr.seq),
                static_cast<unsigned int>(cmd.node_id));
      return false;
    }

    const bool ok = _radio.sendToWait(loraPayload, loraLen, cmd.node_id);
    LOG_INFO("base", "tx_cmd_reset seq=%u node=%u reset_type=%u uart_format=%s lora_len=%u result=%s",
             static_cast<unsigned int>(cmdHdr.seq),
             static_cast<unsigned int>(cmd.node_id),
             static_cast<unsigned int>(cmd.reset_type),
             legacyNoCrc ? "legacy_no_crc" : "lora_crc",
             static_cast<unsigned int>(loraLen), ok ? "OK" : "FAIL");
    return ok;
  }

  LOG_WARN("base", "uart_cmd_unsupported type=%s code=0x%02X",
           pktTypeName(hdr.pkt_type), static_cast<unsigned int>(hdr.pkt_type));

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
        LOG_WARN("base", "uart_frame_err reason=zero_length count=%lu",
                 static_cast<unsigned long>(_uartFrameErrorCount));
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
        LOG_WARN(
            "base",
            "uart_frame_err reason=crc_mismatch count=%lu expected=0x%02X got=0x%02X",
            static_cast<unsigned long>(_uartFrameErrorCount),
            static_cast<unsigned int>(expected),
            static_cast<unsigned int>(_uartRx.crc));
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

  LOG_INFO(
      "base",
      "health rx_fwd=%lu cmd_fwd=%lu awaken_rx=%lu bundle_rx=%lu status_rx=%lu full_rx=%lu calib_rx=%lu cmd_ack_rx=%lu raw_rx=%lu sync_tx=%lu ack_tx=%lu time_src=%s jetson_sync_age_ms=%lu rx_fail=%lu last_rx_ms_ago=%lu uart_err=%lu",
      static_cast<unsigned long>(_rxForwardCount),
      static_cast<unsigned long>(_cmdForwardCount),
      static_cast<unsigned long>(_awakenRxCount),
      static_cast<unsigned long>(_bundleRxCount),
      static_cast<unsigned long>(_statusRxCount),
      static_cast<unsigned long>(_fullStateRxCount),
      static_cast<unsigned long>(_calibrationDataRxCount),
      static_cast<unsigned long>(_cmdAckRxCount),
      static_cast<unsigned long>(_rawRxCount),
      static_cast<unsigned long>(_timeSyncTxCount),
      static_cast<unsigned long>(_ackTxCount),
      _hasJetsonTime ? "jetson" : "base_local",
      static_cast<unsigned long>(_hasJetsonTime ? (now - _localMsAtJetsonUpdate)
                                                : 0xFFFFFFFFu),
      static_cast<unsigned long>(_radioReceiveFailCount),
      static_cast<unsigned long>(lastRxAgoMs),
      static_cast<unsigned long>(_uartFrameErrorCount));

  _lastHealthLogMs = now;
}
