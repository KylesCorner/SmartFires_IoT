#include "app/SmartFiresNodeApp.h"
#include <Arduino.h>

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

  // Broadcast AWAKEN immediately — sensors stay idle until TIME_SYNC arrives.
  sendAwakenPacket();
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
      sendAwakenPacket();
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

void SmartFiresNodeApp::sendAwakenPacket() {
  uint8_t buf[BinaryPacket::kAwakenLoRaSize];
  const uint8_t seqUsed = _awakenSeq;
  const uint8_t len = BinaryPacket::encodeAwakenPayload(
      _cfg.nodeId, _awakenSeq++, buf, sizeof(buf));
  if (len > 0) {
    _radio.enqueueTelemetry(buf, len);
    Serial.print("[App] AWAKEN enqueued seq=");
    Serial.println(seqUsed);
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
