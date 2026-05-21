#include "app/SmartFiresNodeApp.h"
#include <Arduino.h>
#include <string.h>

#ifndef UNIT_TEST
#define APP_LOG(msg) Serial.println(msg)
#else
#define APP_LOG(msg) ((void)0)
#endif

SmartFiresNodeApp::SmartFiresNodeApp(
    const Config &cfg, IClock &clock, DutyCycleController &duty,
    PacketHandler &packetHandler, TdmaRadioService &radio, TdmaClock &tdmaClock,
    ISensor **sensors, size_t sensorCount, BatteryMonitor *battery)
    : _cfg(cfg), _clock(clock), _duty(duty), _packetHandler(packetHandler),
      _radio(radio), _tdmaClock(tdmaClock), _sensors(sensors),
      _sensorCount(sensorCount), _battery(battery) {}

bool SmartFiresNodeApp::begin() {
  if (_cfg.enableBattery && _battery) {
    if (!_battery->begin()) {
      APP_LOG("[App] Battery begin failed");
      return false;
    }
  }

  if (!_duty.begin()) {
    APP_LOG("[App] DutyCycle begin failed");
    return false;
  }

  if (!_radio.begin()) {
    APP_LOG("[App] Radio begin failed");
    return false;
  }

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

  APP_LOG("[App] Waiting for TIME_SYNC...");
  return true;
}

void SmartFiresNodeApp::update() {
  if (!_initialized) {
    Serial.println("App not initalized. Could not update");
    return;
  }

  // Always poll radio so TIME_SYNC packets are processed.
  _radio.update();
  handleIncomingCommands();
  updateCalibrationMode();

  if (_cfg.nodeId != _radio.nodeId()) {
    _cfg.nodeId = _radio.nodeId();
    _packetHandler.setNodeId(_cfg.nodeId);
    Serial.print("[App] ASSIGNED node=");
    Serial.print(_cfg.nodeId);
    Serial.print(" slot=");
    Serial.println(_tdmaClock.mySlot());
  }

  const bool hasFreshSync = _tdmaClock.hasSync() && !_tdmaClock.syncStale();
  if (hasFreshSync && !_syncActive) {
    _syncActive = true;
    _awakenOnlyNotified = false;
    _packetHandler.resetStatusTimer();
    Serial.print("[App] TIME_SYNC acquired sessionMs=");
    Serial.print(_tdmaClock.sessionNowMs());
    Serial.print(" current_slot=");
    Serial.print(_tdmaClock.currentSlotNumber());
    Serial.print(" my_slot=");
    Serial.println(_tdmaClock.mySlot());
  } else if (!hasFreshSync && _syncActive) {
    _syncActive = false;
    _awakenOnlyNotified = false;
    APP_LOG("[App] TIME_SYNC stale/lost -- resuming AWAKEN retry");
  }

  // Hold off sensing until the base station has provided the session clock.
  if (!hasFreshSync) {
    const uint32_t now = _clock.millis();
    if (now - _awakenLastSentMs >= kAwakenIntervalMs) {
      sendAwakenHandshake();
      _awakenLastSentMs = now;
      APP_LOG("[App] Waiting for TIME_SYNC -- AWAKEN retried");
    }
    return;
  }

  if (_cfg.awakenOnlyMode) {
    if (!_awakenOnlyNotified) {
      APP_LOG("[App] AWAKEN_ONLY mode -- telemetry suppressed");
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
    _battery->sample();
  }

  if (_duty.telemetryReady()) {
    const SensorSnapshot snap = buildSnapshot();
    _packetHandler.push(snap);
    APP_LOG("[App] Telemetry ready -- snapshot built");

    if (_packetHandler.statusPacketReady()) {
      uint8_t buf[BinaryPacket::kStatusLoRaSize];
      const uint8_t len = _packetHandler.takeStatusPacket(buf, sizeof(buf));
      if (len > 0) {
        _radio.enqueueTelemetry(buf, len);
        APP_LOG("[App] STATUS enqueued");
      }
    }

    if (_packetHandler.bundleReady()) {
      uint8_t buf[BinaryPacket::kMaxBundleLoRaSize];
      const uint8_t len = _packetHandler.takeBundle(buf, sizeof(buf));
      if (len > 0) {
        _radio.enqueueTelemetry(buf, len);
        APP_LOG("[App] BUNDLE enqueued");
      }
    }

    _duty.markTelemetrySent();
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
    Serial.print("[App] AWAKEN direct seq=");
    Serial.print(seqUsed);
    Serial.print(" uid=0x");
    Serial.println(_cfg.deviceUidHash, HEX);
    if (!ok) {
      APP_LOG("[App] AWAKEN direct send failed");
    }
  }
}

SensorSnapshot SmartFiresNodeApp::buildSnapshot() const {
  SensorSnapshot snap;
  snap.sessionTimeMs = _tdmaClock.sessionNowMs();

  for (size_t i = 0; i < _sensorCount; ++i) {
    if (_sensors[i]) {
      _sensors[i]->fillSnapshot(snap);
    }
  }

  if (_cfg.enableBattery && _battery) {
    const BatteryMonitor::Reading &batt = _battery->reading();
    if (batt.valid) {
      snap.batteryMv = static_cast<uint16_t>(batt.batteryVolts * 1000.0f);
      snap.batteryPct = static_cast<uint8_t>(batt.percent);
      snap.batteryValid = true;
    }
  }

  return snap;
}
