#include "app/SmartFiresNodeApp.h"

#include "logging/DebugLogger.h"

#include <Arduino.h>
#include <string.h>

namespace {

const char *boolName(bool v) { return v ? "true" : "false"; }

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

  _initialized = true;

  for (size_t i = 0; i < _sensorCount; ++i) {
    if (_sensors[i] && strcmp(_sensors[i]->name(), "imu") == 0) {
      _imuSensor = _sensors[i];
      break;
    }
  }

  if (_imuSensor) {
    APP_LOG("[App] IMU sensor detected for calibration flow");
  } else {
    APP_LOG("[App] WARNING: IMU sensor not found; calibration commands will fail");
  }

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
  updateCalibrationMode();

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

  if (_calState != CalibrationState::Idle) {
    if (!_calTelemetrySuppressedLogged) {
      APP_LOG("[App] Calibration active -- telemetry enqueue temporarily suppressed");
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

    _packetHandler.push(snap);

    LOG_INFO("app",
             "telemetry_ready snapshot_built session_ms=%lu battery_valid=%u "
             "battery_mv=%u battery_pct=%u",
             static_cast<unsigned long>(snap.sessionTimeMs),
             snap.batteryValid ? 1 : 0,
             static_cast<unsigned int>(snap.batteryMv),
             static_cast<unsigned int>(snap.batteryPct));

    if (_packetHandler.statusPacketReady()) {
      uint8_t buf[BinaryPacket::kStatusLoRaSize];
      const uint8_t len = _packetHandler.takeStatusPacket(buf, sizeof(buf));

      if (len > 0) {
        _radio.enqueueTelemetry(buf, len);

        LOG_INFO("packet", "status_enqueued len=%u",
                 static_cast<unsigned int>(len));
      } else {
        LOG_WARN("packet", "status_ready_but_take_returned_zero");
      }
    }

    if (_packetHandler.bundleReady()) {
      uint8_t buf[BinaryPacket::kMaxBundleLoRaSize];
      const uint8_t len = _packetHandler.takeBundle(buf, sizeof(buf));

      if (len > 0) {
        _radio.enqueueTelemetry(buf, len);

        LOG_INFO("packet", "bundle_enqueued len=%u",
                 static_cast<unsigned int>(len));
      } else {
        LOG_WARN("packet", "bundle_ready_but_take_returned_zero");
      }
    }

    _duty.markTelemetrySent();

    LOG_DEBUG("app", "telemetry_marked_sent");
  }
}

void SmartFiresNodeApp::handleIncomingCommands() {
  TdmaRadioService::ReceivedCommand cmd = {};
  while (_radio.takePendingCommand(cmd)) {
    BinaryPacket::PktHeader hdr = {};
    if (cmd.len < sizeof(BinaryPacket::PktHeader)) {
      continue;
    }
    memcpy(&hdr, cmd.data, sizeof(BinaryPacket::PktHeader));

    if (hdr.magic != BinaryPacket::PKT_MAGIC) {
      continue;
    }

    Serial.print("[App] CMD received type=0x");
    Serial.print(hdr.pkt_type, HEX);
    Serial.print(" seq=");
    Serial.print(hdr.seq);
    Serial.print(" from=");
    Serial.print(cmd.from);
    Serial.print(" len=");
    Serial.println(cmd.len);

    if (hdr.pkt_type == BinaryPacket::PKT_CMD_CALIBRATE) {
      BinaryPacket::CmdCalibratePayload payload = {};
      BinaryPacket::PktHeader ignored = {};
      if (!BinaryPacket::decodeCmdCalibrate(cmd.data, cmd.len, ignored, payload)) {
        APP_LOG("[App] CMD_CALIBRATE decode failed");
        continue;
      }

      if (payload.node_id != _cfg.nodeId) {
        Serial.print("[App] CMD_CALIBRATE ignored target=");
        Serial.print(payload.node_id);
        Serial.print(" local=");
        Serial.println(_cfg.nodeId);
        continue;
      }

      if (!_imuSensor) {
        APP_LOG("[App] CMD_CALIBRATE rejected: IMU unavailable");
        sendCmdAck(BinaryPacket::PKT_CMD_CALIBRATE, 0x02);
        continue;
      }

      if (_calState != CalibrationState::Idle) {
        APP_LOG("[App] CMD_CALIBRATE received during active calibration; restarting session");
      }

      _calDurationS = payload.duration_s == 0 ? 60 : payload.duration_s;
      resetCalibrationStats();
      _calStartMs = _clock.millis();
      _lastCalSampleMs = 0;
      _lastCalUploadAttemptMs = 0;
      _lastCalProgressLogMs = _calStartMs;
      _calUploadAttemptCount = 0;
      _calState = CalibrationState::Calibrating;
      if (_imuSensor) {
        _imuSensor->wake();
      }
      const bool ackOk = sendCmdAck(BinaryPacket::PKT_CMD_CALIBRATE, 0x01);

      Serial.print("[App] CALIBRATE start duration_s=");
      Serial.print(_calDurationS);
      Serial.print(" node=");
      Serial.print(_cfg.nodeId);
      Serial.print(" ack=");
      Serial.println(ackOk ? "OK" : "FAIL");
    } else if (hdr.pkt_type == BinaryPacket::PKT_CMD_RESET) {
      BinaryPacket::CmdResetPayload payload = {};
      BinaryPacket::PktHeader ignored = {};
      if (!BinaryPacket::decodeCmdReset(cmd.data, cmd.len, ignored, payload)) {
        APP_LOG("[App] CMD_RESET decode failed");
        continue;
      }

      if (payload.node_id != _cfg.nodeId) {
        Serial.print("[App] CMD_RESET ignored target=");
        Serial.print(payload.node_id);
        Serial.print(" local=");
        Serial.println(_cfg.nodeId);
        continue;
      }

      const bool ackOk = sendCmdAck(BinaryPacket::PKT_CMD_RESET, 0x00);
      Serial.print("[App] RESET cmd received type=");
      Serial.print(payload.reset_type);
      Serial.print(" ack=");
      Serial.println(ackOk ? "OK" : "FAIL");
#ifndef UNIT_TEST
      NVIC_SystemReset();
#endif
    }
  }
}

void SmartFiresNodeApp::updateCalibrationMode() {
  if (_calState == CalibrationState::Idle) {
    return;
  }

  if (_calState == CalibrationState::Calibrating) {
    maybeCaptureCalibrationSample();
    const uint32_t now = _clock.millis();
    if (now - _lastCalProgressLogMs >= 5000u) {
      _lastCalProgressLogMs = now;
      Serial.print("[App] CAL progress elapsed_ms=");
      Serial.print(calibrationElapsedMs());
      Serial.print(" samples=");
      Serial.print(_calStats.n);
      Serial.print(" min=[");
      Serial.print(_calStats.minV[0], 2);
      Serial.print(",");
      Serial.print(_calStats.minV[1], 2);
      Serial.print(",");
      Serial.print(_calStats.minV[2], 2);
      Serial.print("] max=[");
      Serial.print(_calStats.maxV[0], 2);
      Serial.print(",");
      Serial.print(_calStats.maxV[1], 2);
      Serial.print(",");
      Serial.print(_calStats.maxV[2], 2);
      Serial.println("]");
    }
    if (calibrationElapsedMs() >= static_cast<uint32_t>(_calDurationS) * 1000u) {
      _calState = CalibrationState::Uploading;
      Serial.print("[App] CALIBRATE done samples=");
      Serial.println(_calStats.n);
    }
    return;
  }

  if (_calState == CalibrationState::Uploading) {
    const uint32_t now = _clock.millis();
    if (_lastCalUploadAttemptMs != 0 && now - _lastCalUploadAttemptMs < 1000u) {
      return;
    }

    _lastCalUploadAttemptMs = now;
    _calUploadAttemptCount++;
    uint8_t status = kCalStatusSuccess;
    if (_calStats.n < 200) {
      status = kCalStatusLowSampleCount;
    }
    Serial.print("[App] CAL upload attempt=");
    Serial.print(_calUploadAttemptCount);
    Serial.print(" status=");
    Serial.print(status);
    Serial.print(" samples=");
    Serial.println(_calStats.n);
    if (sendCalibrationData(status)) {
      _calState = CalibrationState::Idle;
      Serial.println("[App] CALIBRATION_DATA sent; calibration mode idle");
    } else {
      APP_LOG("[App] CALIBRATION_DATA send failed; retrying");
    }
  }
}

bool SmartFiresNodeApp::maybeCaptureCalibrationSample() {
  if (!_imuSensor) {
    APP_LOG("[App] CAL sample skipped: IMU missing");
    return false;
  }

  const uint32_t now = _clock.millis();
  if (_lastCalSampleMs != 0 && now - _lastCalSampleMs < 100u) {
    return false;
  }

  _lastCalSampleMs = now;

  _imuSensor->service();
  if (!_imuSensor->sample()) {
    return false;
  }

  if (_imuSensor->readingSize() != sizeof(Icm20948Sensor::Reading)) {
    return false;
  }

  const auto *reading =
      static_cast<const Icm20948Sensor::Reading *>(_imuSensor->readingData());
  if (!reading || !reading->valid) {
    return false;
  }

  updateCalibrationStats(reading->magX, reading->magY, reading->magZ);
  return true;
}

void SmartFiresNodeApp::resetCalibrationStats() {
  _calStats = {};
}

void SmartFiresNodeApp::updateCalibrationStats(float mx, float my, float mz) {
  const float sample[3] = {mx, my, mz};
  const uint16_t nPrev = _calStats.n;
  const uint16_t nNew = static_cast<uint16_t>(nPrev + 1);
  _calStats.n = nNew;

  if (!_calStats.minMaxInitialized) {
    _calStats.minV[0] = _calStats.maxV[0] = mx;
    _calStats.minV[1] = _calStats.maxV[1] = my;
    _calStats.minV[2] = _calStats.maxV[2] = mz;
    _calStats.minMaxInitialized = true;
  } else {
    for (uint8_t i = 0; i < 3; ++i) {
      if (sample[i] < _calStats.minV[i]) _calStats.minV[i] = sample[i];
      if (sample[i] > _calStats.maxV[i]) _calStats.maxV[i] = sample[i];
    }
  }

  float delta[3] = {
      mx - _calStats.mean[0],
      my - _calStats.mean[1],
      mz - _calStats.mean[2],
  };

  _calStats.mean[0] += delta[0] / static_cast<float>(nNew);
  _calStats.mean[1] += delta[1] / static_cast<float>(nNew);
  _calStats.mean[2] += delta[2] / static_cast<float>(nNew);

  float delta2[3] = {
      mx - _calStats.mean[0],
      my - _calStats.mean[1],
      mz - _calStats.mean[2],
  };

  _calStats.m2_xx += delta[0] * delta2[0];
  _calStats.m2_yy += delta[1] * delta2[1];
  _calStats.m2_zz += delta[2] * delta2[2];
  _calStats.m2_xy += delta[0] * delta2[1];
  _calStats.m2_xz += delta[0] * delta2[2];
  _calStats.m2_yz += delta[1] * delta2[2];
}

bool SmartFiresNodeApp::sendCmdAck(uint8_t cmdType, uint8_t status) {
  BinaryPacket::CmdAckPayload ack = {};
  ack.cmd_type = cmdType;
  ack.uid_hash = _cfg.deviceUidHash;
  ack.status = status;

  uint8_t buf[BinaryPacket::kCmdAckLoRaSize] = {};
  const uint8_t len =
      BinaryPacket::encodeCmdAckPayload(_cfg.nodeId, _cmdSeq++, ack, buf, sizeof(buf));
  if (len == 0) {
    return false;
  }

  return _radio.sendImmediate(buf, len, true);
}

bool SmartFiresNodeApp::sendCalibrationData(uint8_t status) {
  BinaryPacket::CalibrationDataPayload payload = {};
  payload.uid_hash = _cfg.deviceUidHash;
  payload.sample_count = _calStats.n;
  payload.mag_mean[0] = _calStats.mean[0];
  payload.mag_mean[1] = _calStats.mean[1];
  payload.mag_mean[2] = _calStats.mean[2];

  const float denom = _calStats.n > 1 ? static_cast<float>(_calStats.n - 1) : 1.0f;
  payload.mag_cov[0] = _calStats.m2_xx / denom;
  payload.mag_cov[1] = _calStats.m2_yy / denom;
  payload.mag_cov[2] = _calStats.m2_zz / denom;
  payload.mag_cov[3] = _calStats.m2_xy / denom;
  payload.mag_cov[4] = _calStats.m2_xz / denom;
  payload.mag_cov[5] = _calStats.m2_yz / denom;

  payload.mag_min[0] = _calStats.minV[0];
  payload.mag_min[1] = _calStats.minV[1];
  payload.mag_min[2] = _calStats.minV[2];
  payload.mag_max[0] = _calStats.maxV[0];
  payload.mag_max[1] = _calStats.maxV[1];
  payload.mag_max[2] = _calStats.maxV[2];
  payload.status = status;

  uint8_t buf[BinaryPacket::kCalibrationDataLoRaSize] = {};
  const uint8_t len = BinaryPacket::encodeCalibrationDataPayload(
      _cfg.nodeId, _cmdSeq++, payload, buf, sizeof(buf));
  if (len == 0) {
    return false;
  }

  return _radio.sendImmediate(buf, len, true);
}

uint32_t SmartFiresNodeApp::calibrationElapsedMs() const {
  return _clock.millis() - _calStartMs;
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
