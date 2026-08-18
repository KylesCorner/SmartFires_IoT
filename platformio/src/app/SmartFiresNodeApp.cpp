// ---
// description: Implements SmartFiresNodeApp's update loop — TIME_SYNC wait/AWAKEN retry, duty-cycle-driven snapshot building, telemetry enqueueing, and incoming CMD_CALIBRATE/CMD_RESET handling.
// role: implementation
// ---
#include "app/SmartFiresNodeApp.h"

#include "calibration/CalibrationDebug.h"
#include "config/SensingConfig.h"
#include "logging/DebugLogger.h"
#include "platform/ResetDiagnostics.h"

#include <Arduino.h>
#include <string.h>

namespace {
  bool dutyPhaseSleepsRadio(
    DutyCyclePhase phase) {
      return
          phase == DutyCyclePhase::CooldownSleeping ||
          phase == DutyCyclePhase::IdleSleeping;
    }

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

// SmartFiresNodeApp::SmartFiresNodeApp(
//     const Config &cfg, IClock &clock, DutyCycleController &duty,
//     PacketHandler &packetHandler, TdmaRadioService &radio, TdmaClock &tdmaClock,
//     ISensor **sensors, size_t sensorCount, BatteryMonitor *battery)
//     : _cfg(cfg), _clock(clock), _duty(duty), _packetHandler(packetHandler),
//       _radio(radio), _tdmaClock(tdmaClock), _sensors(sensors),
//       _sensorCount(sensorCount), _battery(battery) {}

SmartFiresNodeApp::SmartFiresNodeApp(
    const Config &cfg,
    IClock &clock,
    DutyCycleController &duty,
    PacketHandler &packetHandler,
    TdmaRadioService &radio,
    TdmaClock &tdmaClock,
    IMcuSleep &mcuSleep,
    ISensor **sensors,
    size_t sensorCount,
    BatteryMonitor *battery)
    : _cfg(cfg),
      _clock(clock),
      _duty(duty),
      _packetHandler(packetHandler),
      _radio(radio),
      _tdmaClock(tdmaClock),
      _mcuSleep(mcuSleep),
      _sensors(sensors),
      _sensorCount(sensorCount),
      _battery(battery) {}
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
  // _radio.update();
    const bool radioShouldSleep =
      dutyPhaseSleepsRadio(_duty.phase()) &&
      !_forceRadioAwake &&
      !radioMustStayAwakeToDrain();

  _radio.setDutySleep(radioShouldSleep);
  _radio.update();

  if (_tdmaClock.consumeSessionChanged()) {
    _packetHandler.reset();
    // A new session id means a new session-clock origin — any pending
    // wake_phase_err prediction is against the old origin, so drop it.
    _predictedValid = false;
    LOG_INFO("app", "session_changed bundle_reset=1 seq_reset=1");
  }

  logWakePhaseErrorOnNextSync();

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
    if (_forceRadioAwake) {
      _forceRadioAwake = false;

      LOG_INFO(
          "sleep",
          "post_standby_sync_reacquired "
          "radio_override_cleared=1");
    }
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
  updateWindowMarkers();

  // The post-standby override only has to hold the radio up for the tail of the
  // sleeping phase. Once the duty controller is awake again, phase-based radio
  // sleep no longer applies, so drop it here — previously this was cleared by
  // the post-sleep resync, which no longer happens now that sync survives
  // standby.
  if (_forceRadioAwake && !_duty.sleeping()) {
    _forceRadioAwake = false;

    LOG_DEBUG("sleep", "radio_override_cleared reason=duty_awake phase=%u",
              static_cast<unsigned int>(_duty.phase()));
  }

  const bool radioShouldSleepAfterDutyUpdate =
    dutyPhaseSleepsRadio(_duty.phase()) &&
    !_forceRadioAwake &&
    !radioMustStayAwakeToDrain();

  _radio.setDutySleep(
      radioShouldSleepAfterDutyUpdate);

  if (maybeEnterTimedMcuSleep()) {
    return;
  }

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
        enqueueTelemetryPayload(buf, len);
      } else {
        LOG_WARN("packet", "status_ready_but_take_returned_zero");
      }
    }

    if (_packetHandler.bundleReady()) {
      if (!_cfg.enableTelemetryTx) {
        LOG_INFO("app",
                 "bundle_tx_disabled bundle_dropped session_ms=%lu",
                 static_cast<unsigned long>(snap.sessionTimeMs));
        _duty.markTelemetrySent();
        return;
      }

      takeAndEnqueueBundle();
    }

    _duty.markTelemetrySent();

    LOG_DEBUG("app", "telemetry_marked_sent");
  }
}

void SmartFiresNodeApp::sendAwakenHandshake() {
  uint8_t buf[BinaryPacket::kAwakenLoRaSize];

  BinaryPacket::AwakenPayload awaken = {};
  awaken.uid_hash = _cfg.deviceUidHash;
  // Carry this boot's reset cause + prior hang zone out to the base/Jetson so a
  // watchdog reboot is attributable on the wire (see ResetDiagnostics). Constant
  // for the life of the boot, so it rides every AWAKEN retry identically.
  awaken.reset_cause = ResetDiagnostics::resetCause();
  awaken.hang_zone   = ResetDiagnostics::hangZone();

  const uint8_t seqUsed = _awakenSeq;

  const uint8_t len = BinaryPacket::encodeAwakenPayload(
      _cfg.nodeId, _awakenSeq++, awaken, buf, sizeof(buf));

  if (len > 0) {
    const bool ok = _radio.sendAwakenHandshake(buf, len);

    LOG_INFO("app",
             "awaken_direct_send seq=%u uid_hash=0x%08lX reset_cause=0x%02X "
             "hang_zone=%u len=%u ok=%u",
             static_cast<unsigned int>(seqUsed),
             static_cast<unsigned long>(_cfg.deviceUidHash),
             static_cast<unsigned int>(awaken.reset_cause),
             static_cast<unsigned int>(awaken.hang_zone),
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

      if (reset.reset_type == 0x01) {
        delay(200);           // let the ACK reach the base over LoRa before the radio drops
        NVIC_SystemReset();   // hard reset — full MCU reboot, never returns
      }

      // Soft reset: drop sync state, flush pending TX, re-enter AWAKEN loop.
      _tdmaClock.reset();
      _radio.flushTelemetryBuffers("cmd_reset_soft");
      _packetHandler.reset();
      _syncActive = false;
      sendAwakenHandshake();
      _awakenLastSentMs = _clock.millis();
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

// True while the node is holding standby off to drain the TX queue (see
// maybeEnterTimedMcuSleep). The duty phase is a sleeping one by then, so
// phase-based radio sleep would otherwise apply — and TdmaRadioService::update()
// returns before drainTxQueue() whenever the radio is duty-slept, meaning the
// queue could never empty and the drain would always burn its full budget.
bool SmartFiresNodeApp::radioMustStayAwakeToDrain() const {
  return _duty.mode() == DutyCycleMode::Timed &&
         _duty.sleeping() &&
         !_mcuSleptThisCycle &&
         _radio.queuedCount() > 0;
}

bool SmartFiresNodeApp::enqueueTelemetryPayload(const uint8_t *buf,
                                               uint8_t len) {
  BinaryPacket::PktHeader hdr = {};
  const bool hasHdr = decodePacketHeader(buf, len, hdr);
  const bool enqueued = _radio.enqueueTelemetry(buf, len);

  if (enqueued) {
    LOG_INFO("packet",
             "enqueue_ok pkt=%s seq=%u len=%u flags=0x%02X queue_depth=%u",
             hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
             static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
             static_cast<unsigned int>(len),
             static_cast<unsigned int>(hasHdr ? hdr.flags : 0),
             static_cast<unsigned int>(_radio.queuedCount()));
  } else {
    LOG_ERROR("packet",
              "enqueue_failed pkt=%s seq=%u len=%u flags=0x%02X queue_depth=%u",
              hasHdr ? pktTypeName(hdr.pkt_type) : "RAW",
              static_cast<unsigned int>(hasHdr ? hdr.seq : 0),
              static_cast<unsigned int>(len),
              static_cast<unsigned int>(hasHdr ? hdr.flags : 0),
              static_cast<unsigned int>(_radio.queuedCount()));
  }

  return enqueued;
}

bool SmartFiresNodeApp::takeAndEnqueueBundle() {
  uint8_t buf[BinaryPacket::kMaxBundleLoRaSize];
  const uint8_t len = _packetHandler.takeBundle(buf, sizeof(buf));

  if (len == 0) {
    LOG_WARN("packet", "bundle_ready_but_take_returned_zero");
    return false;
  }

  return enqueueTelemetryPayload(buf, len);
}

// Stamps PktHeader::flags so the receiver can bound each Timed active window.
// Entering ActiveSampling opens a window (WINDOW_FIRST on its first bundle);
// leaving it closes the window, which force-encodes the partial bundle that
// would otherwise sit in the accumulator across the whole MCU standby and
// marks it WINDOW_LAST.
void SmartFiresNodeApp::updateWindowMarkers() {
  const DutyCyclePhase phase = _duty.phase();

  if (phase == _lastDutyPhase) {
    return;
  }

  const DutyCyclePhase previous = _lastDutyPhase;
  _lastDutyPhase = phase;

  if (phase == DutyCyclePhase::ActiveSampling) {
    _packetHandler.beginWindow();
    return;
  }

  if (previous != DutyCyclePhase::ActiveSampling) {
    return;
  }

  if (!_packetHandler.flushWindow()) {
    return;
  }

  if (!_cfg.enableTelemetryTx) {
    LOG_INFO("app", "window_flush_tx_disabled bundle_dropped=1");
    return;
  }

  LOG_INFO("app", "window_flush bundle_ready=1");
  takeAndEnqueueBundle();
}

// rtc-subsecond-sleep instrumentation. Phase 1 compared the sleep-compensated
// clock against the forced post-standby resync; with sync now preserved across
// standby (Phase 2), the comparison point is the next TIME_SYNC that arrives on
// its own. applySync() overwrites the sync origin in place, so the prediction
// has to be evaluated on the same tick the origin moves — hence watching
// syncLocalMs() rather than a sync-acquired edge that no longer fires.
void SmartFiresNodeApp::logWakePhaseErrorOnNextSync() {
  if (!_predictedValid || !_tdmaClock.hasSync()) {
    return;
  }

  if (_tdmaClock.syncLocalMs() == _predictedSyncLocalMs) {
    return;
  }

  _predictedValid = false;

  const uint32_t actualMs = _tdmaClock.sessionNowMs();
  const uint32_t predictedMs = _predictedSessionOffsetMs + _clock.millis();
  const int32_t errMs = static_cast<int32_t>(actualMs - predictedMs);

  LOG_INFO("sleep",
           "wake_phase_err predicted_ms=%lu actual_ms=%lu err_ms=%ld "
           "guard_ms=%lu",
           static_cast<unsigned long>(predictedMs),
           static_cast<unsigned long>(actualMs),
           static_cast<long>(errMs),
           static_cast<unsigned long>(NetworkConfig::kGuardMs));
}

bool SmartFiresNodeApp::maybeEnterTimedMcuSleep() {
  if (_duty.mode() != DutyCycleMode::Timed ||
      !_duty.sleeping()) {
    _mcuSleptThisCycle = false;
    _txDrainDeadlineValid = false;
    return false;
  }

  if (_mcuSleptThisCycle) {
    return false;
  }

  // The window-flush bundle is enqueued the moment the active window closes,
  // but it still has to wait for this node's TDMA slot to come around. Going
  // to standby now would park it in the queue for the entire sleep, so stay
  // awake until the queue drains — bounded, since a slot that never opens
  // (base offline) must not stall the duty cycle indefinitely.
  if (_radio.queuedCount() > 0) {
    const uint32_t now = _clock.millis();

    if (!_txDrainDeadlineValid) {
      _txDrainDeadlineValid = true;
      _txDrainDeadlineMs =
          now + SensingConfig::DutyCycle::kMaxTxDrainBeforeStandbyMs;
    }

    if (static_cast<int32_t>(_txDrainDeadlineMs - now) > 0) {
      return false;
    }

    LOG_WARN("sleep",
             "tx_drain_timeout queued=%u drain_budget_ms=%lu sleeping_anyway=1",
             static_cast<unsigned int>(_radio.queuedCount()),
             static_cast<unsigned long>(
                 SensingConfig::DutyCycle::kMaxTxDrainBeforeStandbyMs));
  }

  _txDrainDeadlineValid = false;

  const uint32_t remainingMs =
      _duty.timedSleepRemainingMs();

  // RTC MODE0 alarms have ~1 ms resolution, so the full remainder can
  // go to standby — only skip when it's too short to be worth the
  // enter/exit overhead.
  const uint32_t standbyMs = remainingMs;

  if (standbyMs <
      SensingConfig::DutyCycle::kMinMcuStandbyMs) {
    return false;
  }

  LOG_INFO(
      "sleep",
      "timed_mcu_sleep_start "
      "remaining_ms=%lu standby_ms=%lu",
      static_cast<unsigned long>(remainingMs),
      static_cast<unsigned long>(standbyMs));

  // NodeApp owns radio sleep. Put the RFM95 down before
  // entering SAMD21 standby.
  _radio.setDutySleep(true);

  const uint32_t elapsedMs =
      _mcuSleep.sleepFor(standbyMs);

  _mcuSleptThisCycle = true;

  // Standby is not time the base was given to answer — the radio was off, so an
  // ACK_SUMMARY sent during it could not have been heard. Exclude it from the
  // pending window's retry/expiry math so the window's unacked bundles (always
  // including the WINDOW_LAST one, whose ack can only arrive in a slot 0 that
  // falls after this sleep begins) survive to be retransmitted during the next
  // warmup instead of being dropped as max_age.
  _radio.notifyMcuStandby(elapsedMs);

  // The RTC MODE0 clock carries the session forward across standby to ~1 ms
  // (rtc-subsecond-sleep Phase 1), well inside the 20 ms guard band, so the
  // session survives the sleep — no reset(), no unslotted AWAKEN handshake,
  // no waiting on a fresh TIME_SYNC before telemetry can resume. Cold boot and
  // genuine lost/stale sync still fall back to AWAKEN via update().
  //
  // Instrumentation: sessionNowMs() already includes the compensation
  // sleepFor() just applied. Storing its offset from the local clock lets the
  // next naturally received TIME_SYNC be compared against it (wake_phase_err
  // in update()) without the awake gap polluting the error.
  if (_syncActive && _tdmaClock.hasSync()) {
    _predictedSessionOffsetMs =
        _tdmaClock.sessionNowMs() - _clock.millis();
    _predictedSyncLocalMs = _tdmaClock.syncLocalMs();
    _predictedValid = true;
  }

  // The duty controller is still technically in a sleeping phase until it sees
  // compensated time. Override phase-based radio sleep so the radio is
  // listening again before the node's next slot.
  _forceRadioAwake = true;
  _radio.setDutySleep(false);

  LOG_INFO(
      "sleep",
      "timed_mcu_sleep_complete "
      "elapsed_ms=%lu sync_preserved=%u radio_override=1",
      static_cast<unsigned long>(elapsedMs),
      _tdmaClock.hasSync() ? 1 : 0);

  return true;
}