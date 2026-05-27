#include "app/SmartFiresNodeApp.h"

#include "calibration/CalibrationDebug.h"
#include "logging/DebugLogger.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace {

const char *boolName(bool v) { return v ? "true" : "false"; }

ISensor *findImuSensor(ISensor **sensors, size_t sensorCount) {
  if (!sensors) {
    return nullptr;
  }

  for (size_t i = 0; i < sensorCount; ++i) {
    if (!sensors[i] || !sensors[i]->name()) {
      continue;
    }

    if (strcmp(sensors[i]->name(), "imu") == 0) {
      return sensors[i];
    }
  }

  return nullptr;
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
           "awaken_only=%s sensor_count=%u",
           static_cast<unsigned int>(_cfg.nodeId),
           static_cast<unsigned long>(_cfg.deviceUidHash),
           boolName(_cfg.enableBattery), boolName(_cfg.awakenOnlyMode),
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

  if (!_radio.begin()) {
    LOG_ERROR("radio", "begin_failed caller=app");
    return false;
  }

  LOG_INFO("radio", "begin_ok caller=app node_id=%u",
           static_cast<unsigned int>(_radio.nodeId()));

  _imuSensor = findImuSensor(_sensors, _sensorCount);

  LOG_INFO("calib", "imu_sensor_discovery found=%u sensor_count=%u",
           _imuSensor ? 1 : 0, static_cast<unsigned int>(_sensorCount));

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

  updateCalibrationMode();

  if (_calState != CalibrationState::Idle) {
    if (!_calTelemetrySuppressedLogged) {
      LOG_INFO("calib", "telemetry_suppressed state=%u",
               static_cast<unsigned int>(_calState));
      _calTelemetrySuppressedLogged = true;
    }

    return;
  }

  _calTelemetrySuppressedLogged = false;

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

      _imuSensor = _imuSensor ? _imuSensor : findImuSensor(_sensors, _sensorCount);
      if (!_imuSensor) {
        LOG_ERROR("calib", "cmd_calibrate_reject reason=no_imu_sensor seq=%u",
                  static_cast<unsigned int>(hdr.seq));
        sendCmdAck(BinaryPacket::PKT_CMD_CALIBRATE, kCalStatusError);
        continue;
      }

      _calDurationS = calibrate.duration_s == 0 ? 60 : calibrate.duration_s;
      _calStartMs = _clock.millis();
      _lastCalSampleMs = 0;
      _calUploadAttemptCount = 0;
      _lastCalUploadAttemptMs = 0;
      _lastCalProgressLogMs = 0;
      _radio.flushTelemetryBuffers("calibration_start");
      _calState = CalibrationState::Calibrating;
      resetCalibrationStats();

      LOG_INFO("calib",
               "cmd_calibrate_start seq=%u duration_s=%u node=%u from=%u rssi=%d",
               static_cast<unsigned int>(hdr.seq),
               static_cast<unsigned int>(_calDurationS),
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

      _calState = CalibrationState::Idle;
      _calUploadAttemptCount = 0;
      resetCalibrationStats();

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

void SmartFiresNodeApp::updateCalibrationMode() {
  if (_calState == CalibrationState::Idle) {
    return;
  }

  if (_calState == CalibrationState::Calibrating) {
    if (_imuSensor) {
      _imuSensor->service();
      if (_imuSensor->ready()) {
        if (!_imuSensor->sample()) {
          LOG_WARN("calib", "imu_sample_failed_during_calibration");
        }
      }
    }

    maybeCaptureCalibrationSample();

    const uint32_t now = _clock.millis();
    if (now - _lastCalProgressLogMs >= 5000) {
      _lastCalProgressLogMs = now;
      LOG_INFO("calib", "calibrating elapsed_ms=%lu duration_s=%u samples=%u",
               static_cast<unsigned long>(calibrationElapsedMs()),
               static_cast<unsigned int>(_calDurationS),
               static_cast<unsigned int>(_calStats.n));
    }

    const uint32_t targetMs = static_cast<uint32_t>(_calDurationS) * 1000UL;
    if (calibrationElapsedMs() < targetMs) {
      return;
    }

    _calState = CalibrationState::Uploading;
    _lastCalUploadAttemptMs = 0;
    _calUploadAttemptCount = 0;

    LOG_INFO("calib", "calibration_window_complete samples=%u",
             static_cast<unsigned int>(_calStats.n));
  }

  if (_calState != CalibrationState::Uploading) {
    return;
  }

  const uint32_t now = _clock.millis();
  if (_calUploadAttemptCount > 0 && now - _lastCalUploadAttemptMs < 1000) {
    return;
  }

  _lastCalUploadAttemptMs = now;
  _calUploadAttemptCount++;

  const uint8_t status = (_calStats.n < 10) ? kCalStatusLowSampleCount
                                            : kCalStatusSuccess;

  if (!sendCalibrationData(status)) {
    LOG_WARN("calib", "calibration_upload_retry attempt=%u status=%s",
             static_cast<unsigned int>(_calUploadAttemptCount),
             CalibrationDebug::statusName(status));

    if (_calUploadAttemptCount >= 5) {
      LOG_ERROR("calib", "calibration_upload_abandon attempts=%u",
                static_cast<unsigned int>(_calUploadAttemptCount));
      _calState = CalibrationState::Idle;
    }

    return;
  }

  LOG_INFO("calib", "calibration_complete status=%s samples=%u attempts=%u",
           CalibrationDebug::statusName(status),
           static_cast<unsigned int>(_calStats.n),
           static_cast<unsigned int>(_calUploadAttemptCount));

  _calState = CalibrationState::Idle;
}

bool SmartFiresNodeApp::maybeCaptureCalibrationSample() {
  if (_calState != CalibrationState::Calibrating || !_imuSensor) {
    return false;
  }

  if (_imuSensor->readingSize() < sizeof(Icm20948Sensor::Reading)) {
    LOG_ERROR("calib", "sample_reject reason=imu_reading_size_mismatch size=%u",
              static_cast<unsigned int>(_imuSensor->readingSize()));
    return false;
  }

  const Icm20948Sensor::Reading *reading =
      static_cast<const Icm20948Sensor::Reading *>(_imuSensor->readingData());

  if (!reading || !reading->valid || reading->timestampMs == 0 ||
      reading->timestampMs == _lastCalSampleMs) {
    return false;
  }

  _lastCalSampleMs = reading->timestampMs;
  updateCalibrationStats(reading->magX, reading->magY, reading->magZ);

  if ((_calStats.n % 25u) == 0u) {
    LOG_DEBUG("calib",
              "sample_capture samples=%u last_mag=[%.3f,%.3f,%.3f] t_ms=%lu",
              static_cast<unsigned int>(_calStats.n), reading->magX,
              reading->magY, reading->magZ,
              static_cast<unsigned long>(reading->timestampMs));
  }

  return true;
}

void SmartFiresNodeApp::resetCalibrationStats() {
  _calStats = CalibrationStats{};
}

void SmartFiresNodeApp::updateCalibrationStats(float mx, float my, float mz) {
  if (!_calStats.minMaxInitialized) {
    _calStats.minV[0] = _calStats.maxV[0] = mx;
    _calStats.minV[1] = _calStats.maxV[1] = my;
    _calStats.minV[2] = _calStats.maxV[2] = mz;
    _calStats.minMaxInitialized = true;
  } else {
    _calStats.minV[0] = fminf(_calStats.minV[0], mx);
    _calStats.minV[1] = fminf(_calStats.minV[1], my);
    _calStats.minV[2] = fminf(_calStats.minV[2], mz);
    _calStats.maxV[0] = fmaxf(_calStats.maxV[0], mx);
    _calStats.maxV[1] = fmaxf(_calStats.maxV[1], my);
    _calStats.maxV[2] = fmaxf(_calStats.maxV[2], mz);
  }

  _calStats.n = static_cast<uint16_t>(_calStats.n + 1u);

  const float n = static_cast<float>(_calStats.n);
  const float dx = mx - _calStats.mean[0];
  const float dy = my - _calStats.mean[1];
  const float dz = mz - _calStats.mean[2];

  _calStats.mean[0] += dx / n;
  _calStats.mean[1] += dy / n;
  _calStats.mean[2] += dz / n;

  const float dx2 = mx - _calStats.mean[0];
  const float dy2 = my - _calStats.mean[1];
  const float dz2 = mz - _calStats.mean[2];

  _calStats.m2_xx += dx * dx2;
  _calStats.m2_yy += dy * dy2;
  _calStats.m2_zz += dz * dz2;
  _calStats.m2_xy += dx * dy2;
  _calStats.m2_xz += dx * dz2;
  _calStats.m2_yz += dy * dz2;
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

bool SmartFiresNodeApp::sendCalibrationData(uint8_t status) {
  BinaryPacket::CalibrationDataPayload calib = {};
  calib.uid_hash = _cfg.deviceUidHash;
  calib.sample_count = _calStats.n;
  calib.mag_mean[0] = _calStats.mean[0];
  calib.mag_mean[1] = _calStats.mean[1];
  calib.mag_mean[2] = _calStats.mean[2];

  float inv = 0.0f;
  if (_calStats.n > 1) {
    inv = 1.0f / static_cast<float>(_calStats.n - 1u);
  }

  calib.mag_cov[0] = _calStats.m2_xx * inv;
  calib.mag_cov[1] = _calStats.m2_yy * inv;
  calib.mag_cov[2] = _calStats.m2_zz * inv;
  calib.mag_cov[3] = _calStats.m2_xy * inv;
  calib.mag_cov[4] = _calStats.m2_xz * inv;
  calib.mag_cov[5] = _calStats.m2_yz * inv;

  calib.mag_min[0] = _calStats.minV[0];
  calib.mag_min[1] = _calStats.minV[1];
  calib.mag_min[2] = _calStats.minV[2];
  calib.mag_max[0] = _calStats.maxV[0];
  calib.mag_max[1] = _calStats.maxV[1];
  calib.mag_max[2] = _calStats.maxV[2];
  calib.status = status;

  const uint8_t seq = _cmdSeq++;
  uint8_t payload[BinaryPacket::kCalibrationDataLoRaSize] = {};
  const uint8_t len =
      BinaryPacket::encodeCalibrationDataPayload(_cfg.nodeId, seq, calib,
                                                 payload, sizeof(payload));
  if (len == 0) {
    LOG_ERROR("calib", "calibration_data_encode_failed seq=%u",
              static_cast<unsigned int>(seq));
    return false;
  }

  const bool ok = _radio.sendImmediate(payload, len, true);

  CalibrationDebug::logCalibrationDataSummary(calib, _cfg.nodeId, seq, "calib");

  LOG_INFO("calib", "calibration_data_tx_result seq=%u status=%s ok=%u",
           static_cast<unsigned int>(seq), CalibrationDebug::statusName(status),
           ok ? 1 : 0);

  return ok;
}

uint32_t SmartFiresNodeApp::calibrationElapsedMs() const {
  return _clock.millis() - _calStartMs;
}