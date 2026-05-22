#include "app/SmartFiresNodeApp.h"

#include "logging/DebugLogger.h"

#include <Arduino.h>

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
