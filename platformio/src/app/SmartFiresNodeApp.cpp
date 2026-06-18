#include "app/SmartFiresNodeApp.h"

#include "calibration/CalibrationDebug.h"
#include "logging/DebugLogger.h"

#include <Arduino.h>
#include <string.h>

namespace {

const char *boolName(bool v) { return v ? "true" : "false"; }

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
  case BinaryPacket::PKT_CMD_ACK:
    return "CMD_ACK";
  default:
    return "UNKNOWN";
  }
}

bool decodePacketHeader(const uint8_t *payload,
                        uint8_t len,
                        BinaryPacket::PktHeader &hdrOut) {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    return false;
  }

  memcpy(&hdrOut, payload, sizeof(BinaryPacket::PktHeader));
  return hdrOut.magic == BinaryPacket::PKT_MAGIC;
}

} // namespace

SmartFiresNodeApp::SmartFiresNodeApp(
    const Config &cfg, IClock &clock, DutyCycleController &duty,
    PacketHandler &packetHandler, TdmaRadioService &radio, TdmaClock &tdmaClock,
    ISensor **sensors, size_t sensorCount, BatteryMonitor *battery)
    : _cfg(cfg), _clock(clock), _duty(duty), _packetHandler(packetHandler),
      _radio(radio), _tdmaClock(tdmaClock), _sensors(sensors),
      _sensorCount(sensorCount), _battery(battery) {}

bool SmartFiresNodeApp::begin() {
  LOG_INFO("app",
           "begin node_id=%u uid_hash=0x%08lX enable_battery=%s "
           "awaken_only=%s bundle_tx=%s sensor_count=%u",
           static_cast<unsigned int>(_cfg.nodeId),
           static_cast<unsigned long>(_cfg.deviceUidHash),
           boolName(_cfg.enableBattery), boolName(_cfg.awakenOnlyMode),
           boolName(_cfg.enableTelemetryTx),
           static_cast<unsigned int>(_sensorCount));

  if (_cfg.enableBattery && _battery) {
    if (!_battery->begin()) {
      LOG_ERROR("battery", "begin_failed caller=app");
      return false;
    }

    LOG_INFO("battery", "begin_ok caller=app");
  } else {
    LOG_DEBUG("battery", "begin_skipped enable_battery=%s battery_ptr=%s",
              boolName(_cfg.enableBattery), _battery ? "present" : "null");
  }

  if (!_duty.begin()) {
    LOG_ERROR("app", "duty_begin_failed");
    return false;
  }

  LOG_INFO("app", "duty_begin_ok");

  _packetHandler.setBundleEncodingEnabled(_cfg.enableTelemetryTx);

  if (!_radio.begin()) {
    LOG_ERROR("radio", "begin_failed caller=app");
    return false;
  }

  LOG_INFO("radio", "begin_ok caller=app node_id=%u",
           static_cast<unsigned int>(_radio.nodeId()));

  _initialized = true;

  // Broadcast AWAKEN immediately — sensors stay idle until TIME_SYNC arrives.
  sendAwakenHandshake();
  _awakenLastSentMs = _clock.millis();

  LOG_INFO("app", "waiting_for_time_sync awaken_interval_ms=%lu",
           static_cast<unsigned long>(kAwakenIntervalMs));

  return true;
}

void SmartFiresNodeApp::update() {
  if (!_initialized) {
    LOG_WARN("app", "update_skipped reason=not_initialized");
    return;
  }

  // Always poll radio so TIME_SYNC packets are processed.
  _radio.update();

  if (_tdmaClock.consumeSessionChanged()) {
    _packetHandler.reset();
    LOG_INFO("app", "session_changed bundle_reset=1 seq_reset=1");
  }

  handleIncomingCommands();

  if (_cfg.nodeId != _radio.nodeId()) {
    const uint8_t oldNodeId = _cfg.nodeId;

    _cfg.nodeId = _radio.nodeId();
    _packetHandler.setNodeId(_cfg.nodeId);

    LOG_INFO("app", "node_assigned old_node=%u new_node=%u slot=%u",
             static_cast<unsigned int>(oldNodeId),
             static_cast<unsigned int>(_cfg.nodeId),
             static_cast<unsigned int>(_tdmaClock.mySlot()));
  }

  const bool hasFreshSync = _tdmaClock.hasSync() && !_tdmaClock.syncStale();

  if (hasFreshSync && !_syncActive) {
    _syncActive = true;
    _awakenOnlyNotified = false;
    _packetHandler.resetStatusTimer();

    LOG_INFO("tdma",
             "time_sync_acquired session_ms=%lu current_slot=%u my_slot=%u",
             static_cast<unsigned long>(_tdmaClock.sessionNowMs()),
             static_cast<unsigned int>(_tdmaClock.currentSlotNumber()),
             static_cast<unsigned int>(_tdmaClock.mySlot()));
  } else if (!hasFreshSync && _syncActive) {
    _syncActive = false;
    _awakenOnlyNotified = false;

    LOG_WARN("tdma", "time_sync_lost_or_stale resuming_awaken_retry=1");
  }

  // Hold off sensing until the base station has provided the session clock.
  if (!hasFreshSync) {
    const uint32_t now = _clock.millis();

    if (now - _awakenLastSentMs >= kAwakenIntervalMs) {
      sendAwakenHandshake();
      _awakenLastSentMs = now;

      LOG_INFO("app", "waiting_for_time_sync awaken_retried=1");
    }

    return;
  }

  if (_cfg.awakenOnlyMode) {
    if (!_awakenOnlyNotified) {
      LOG_WARN("app", "awaken_only_mode telemetry_suppressed=1");
      _awakenOnlyNotified = true;
    }

    return;
  }

  _duty.update();

  if (_cfg.enableBattery && _battery && _battery->ready()) {
    if (_battery->sample()) {
      LOG_DEBUG("battery", "sample_ok caller=app");
    } else {
      LOG_WARN("battery", "sample_failed caller=app");
    }
  }

  if (_duty.telemetryReady()) {
    const SensorSnapshot snap = buildSnapshot();

    // Must precede push() so the STATUS encoder sees current counters.
    _packetHandler.setLinkStats(_radio.retransmitCount(), _radio.failedSendCount());
    const bool bundleReadyFromPush = _packetHandler.push(snap);

    LOG_INFO("app",
             "telemetry_ready snapshot_built session_ms=%lu battery_valid=%u "
             "battery_mv=%u battery_pct=%u",
             static_cast<unsigned long>(snap.sessionTimeMs),
             snap.batteryValid ? 1 : 0,
             static_cast<unsigned int>(snap.batteryMv),
             static_cast<unsigned int>(snap.batteryPct));

    LOG_DEBUG("packet",
              "handler_push_done status_ready=%u bundle_ready=%u push_bundle_ready=%u",
              _packetHandler.statusPacketReady() ? 1 : 0,
              _packetHandler.bundleReady() ? 1 : 0,
              bundleReadyFromPush ? 1 : 0);

    if (_packetHandler.statusPacketReady()) {
      uint8_t buf[BinaryPacket::kStatusLoRaSize];
      const uint8_t len = _packetHandler.takeStatusPacket(buf, sizeof(buf));

      if (len > 0) {
        BinaryPacket::PktHeader hdr = {};
        const bool hasHdr = decodePacketHeader(buf, len, hdr);
        const bool enqueued = _radio.enqueueTelemetry(buf, len);

        if (enqueued) {
          LOG_INFO("packet",
                   "enqueue_ok pkt=%s seq=%u len=%u queue_depth=%u",
                   hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
                   static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
                   static_cast<unsigned int>(len),
                   static_cast<unsigned int>(_radio.queuedCount()));
        } else {
          LOG_ERROR("packet",
                    "enqueue_failed pkt=%s seq=%u len=%u queue_depth=%u",
                    hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
                    static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
                    static_cast<unsigned int>(len),
                    static_cast<unsigned int>(_radio.queuedCount()));
        }
      } else {
        LOG_WARN("packet", "status_ready_but_take_returned_zero");
      }
    }

    if (_packetHandler.bundleReady()) {
      uint8_t buf[BinaryPacket::kMaxBundleLoRaSize];
      const uint8_t len = _packetHandler.takeBundle(buf, sizeof(buf));

      if (len > 0) {
        if (!_cfg.enableTelemetryTx) {
          LOG_INFO("app",
                   "bundle_tx_disabled bundle_dropped len=%u session_ms=%lu",
                   static_cast<unsigned int>(len),
                   static_cast<unsigned long>(snap.sessionTimeMs));
          _duty.markTelemetrySent();
          return;
        }

        BinaryPacket::PktHeader hdr = {};
        const bool hasHdr = decodePacketHeader(buf, len, hdr);
        const bool enqueued = _radio.enqueueTelemetry(buf, len);

        if (enqueued) {
          LOG_INFO("packet",
                   "enqueue_ok pkt=%s seq=%u len=%u queue_depth=%u",
                   hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
                   static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
                   static_cast<unsigned int>(len),
                   static_cast<unsigned int>(_radio.queuedCount()));
        } else {
          LOG_ERROR("packet",
                    "enqueue_failed pkt=%s seq=%u len=%u queue_depth=%u",
                    hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
                    static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
                    static_cast<unsigned int>(len),
                    static_cast<unsigned int>(_radio.queuedCount()));
        }
      } else {
        LOG_WARN("packet", "bundle_ready_but_take_returned_zero");
      }
    }

    _duty.markTelemetrySent();

    LOG_DEBUG("app", "telemetry_marked_sent");
  }
}

void SmartFiresNodeApp::sendAwakenHandshake() {
  uint8_t buf[BinaryPacket::kAwakenLoRaSize];

  BinaryPacket::AwakenPayload awaken = {};
  awaken.uid_hash = _cfg.deviceUidHash;

  const uint8_t seqUsed = _awakenSeq;

  const uint8_t len = BinaryPacket::encodeAwakenPayload(
      _cfg.nodeId, _awakenSeq++, awaken, buf, sizeof(buf));

  if (len > 0) {
    const bool ok = _radio.sendAwakenHandshake(buf, len);

    LOG_INFO("app",
             "awaken_direct_send seq=%u uid_hash=0x%08lX len=%u ok=%u",
             static_cast<unsigned int>(seqUsed),
             static_cast<unsigned long>(_cfg.deviceUidHash),
             static_cast<unsigned int>(len), ok ? 1 : 0);

    if (!ok) {
      LOG_WARN("app", "awaken_direct_send_failed seq=%u",
               static_cast<unsigned int>(seqUsed));
    }
  } else {
    LOG_ERROR("app",
              "awaken_encode_failed node_id=%u seq=%u uid_hash=0x%08lX",
              static_cast<unsigned int>(_cfg.nodeId),
              static_cast<unsigned int>(seqUsed),
              static_cast<unsigned long>(_cfg.deviceUidHash));
  }
}

SensorSnapshot SmartFiresNodeApp::buildSnapshot() const {
  SensorSnapshot snap;
  snap.sessionTimeMs = _tdmaClock.sessionNowMs();

  LOG_DEBUG("app", "snapshot_session_ms=%lu",
            static_cast<unsigned long>(snap.sessionTimeMs));

  LOG_DEBUG("app", "build_snapshot_start session_ms=%lu sensor_count=%u",
            static_cast<unsigned long>(snap.sessionTimeMs),
            static_cast<unsigned int>(_sensorCount));

  for (size_t i = 0; i < _sensorCount; ++i) {
    if (_sensors[i]) {
      _sensors[i]->fillSnapshot(snap);

      LOG_TRACE("app", "snapshot_fill sensor_index=%u sensor=%s",
                static_cast<unsigned int>(i), _sensors[i]->name());
    } else {
      LOG_WARN("app", "snapshot_fill_skipped null_sensor index=%u",
               static_cast<unsigned int>(i));
    }
  }

  if (_cfg.enableBattery && _battery) {
    const BatteryMonitor::Reading &batt = _battery->reading();

    if (batt.valid) {
      snap.batteryMv = static_cast<uint16_t>(batt.batteryVolts * 1000.0f);
      snap.batteryPct = static_cast<uint8_t>(batt.percent);
      snap.batteryValid = true;

      LOG_DEBUG("battery", "snapshot_fill battery_mv=%u battery_pct=%u",
                static_cast<unsigned int>(snap.batteryMv),
                static_cast<unsigned int>(snap.batteryPct));
    } else {
      LOG_DEBUG("battery", "snapshot_fill_skipped reason=invalid_reading");
    }
  }

  LOG_DEBUG("app",
            "build_snapshot_done session_ms=%lu battery_valid=%u sensor_flags=0x%04X",
            static_cast<unsigned long>(snap.sessionTimeMs),
            snap.batteryValid ? 1 : 0,
            static_cast<unsigned int>(snap.sensorFlags));

  return snap;
}

void SmartFiresNodeApp::handleIncomingCommands() {
  TdmaRadioService::ReceivedCommand cmd = {};

  while (_radio.takePendingCommand(cmd)) {
    BinaryPacket::PktHeader hdr = {};
    const bool hasHdr = decodePacketHeader(cmd.data, cmd.len, hdr);

    if (!hasHdr) {
      LOG_WARN("calib", "cmd_reject reason=invalid_header len=%u",
               static_cast<unsigned int>(cmd.len));
      continue;
    }

    if (hdr.pkt_type == BinaryPacket::PKT_CMD_CALIBRATE) {
      BinaryPacket::PktHeader ignored = {};
      BinaryPacket::CmdCalibratePayload calibrate = {};

      if (!BinaryPacket::decodeCmdCalibrate(cmd.data, cmd.len, ignored,
                                            calibrate)) {
        LOG_WARN("calib", "cmd_calibrate_decode_failed len=%u",
                 static_cast<unsigned int>(cmd.len));
        continue;
      }

      if (calibrate.node_id != _cfg.nodeId) {
        LOG_WARN("calib",
                 "cmd_calibrate_skip target_node=%u local_node=%u seq=%u",
                 static_cast<unsigned int>(calibrate.node_id),
                 static_cast<unsigned int>(_cfg.nodeId),
                 static_cast<unsigned int>(hdr.seq));
        continue;
      }

      LOG_INFO("calib",
               "cmd_calibrate_rx seq=%u node=%u from=%u rssi=%d dmp_self_calibrates=1",
               static_cast<unsigned int>(hdr.seq),
               static_cast<unsigned int>(_cfg.nodeId),
               static_cast<unsigned int>(cmd.from), static_cast<int>(cmd.rssi));

      sendCmdAck(BinaryPacket::PKT_CMD_CALIBRATE, kCalStatusSuccess);
      continue;
    }

    if (hdr.pkt_type == BinaryPacket::PKT_CMD_RESET) {
      BinaryPacket::PktHeader ignored = {};
      BinaryPacket::CmdResetPayload reset = {};

      if (!BinaryPacket::decodeCmdReset(cmd.data, cmd.len, ignored, reset)) {
        LOG_WARN("calib", "cmd_reset_decode_failed len=%u",
                 static_cast<unsigned int>(cmd.len));
        continue;
      }

      if (reset.node_id != _cfg.nodeId) {
        LOG_WARN("calib", "cmd_reset_skip target_node=%u local_node=%u seq=%u",
                 static_cast<unsigned int>(reset.node_id),
                 static_cast<unsigned int>(_cfg.nodeId),
                 static_cast<unsigned int>(hdr.seq));
        continue;
      }

      LOG_INFO("calib", "cmd_reset_apply seq=%u reset_type=%u",
               static_cast<unsigned int>(hdr.seq),
               static_cast<unsigned int>(reset.reset_type));

      sendCmdAck(BinaryPacket::PKT_CMD_RESET, kCalStatusSuccess);
      continue;
    }

    LOG_DEBUG("calib", "cmd_ignore type=0x%02X seq=%u",
              static_cast<unsigned int>(hdr.pkt_type),
              static_cast<unsigned int>(hdr.seq));
  }
}

bool SmartFiresNodeApp::sendCmdAck(uint8_t cmdType, uint8_t status) {
  BinaryPacket::CmdAckPayload ack = {};
  ack.cmd_type = cmdType;
  ack.uid_hash = _cfg.deviceUidHash;
  ack.status = status;

  const uint8_t seq = _cmdSeq++;
  uint8_t payload[BinaryPacket::kCmdAckLoRaSize] = {};
  const uint8_t len =
      BinaryPacket::encodeCmdAckPayload(_cfg.nodeId, seq, ack, payload,
                                        sizeof(payload));
  if (len == 0) {
    LOG_ERROR("calib", "cmd_ack_encode_failed cmd=%s status=%s",
              CalibrationDebug::cmdTypeName(cmdType),
              CalibrationDebug::statusName(status));
    return false;
  }

  const bool ok = _radio.sendImmediate(payload, len, true);
  CalibrationDebug::logCmdAckSummary(ack, _cfg.nodeId, seq, "calib");

  LOG_INFO("calib", "cmd_ack_tx_result cmd=%s status=%s ok=%u",
           CalibrationDebug::cmdTypeName(cmdType),
           CalibrationDebug::statusName(status), ok ? 1 : 0);

  return ok;
}

