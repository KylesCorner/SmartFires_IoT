#include "app/SmartFiresNodeApp.h"
#include <Arduino.h>

SmartFiresNodeApp::SmartFiresNodeApp(
    const Config &cfg,
    IClock &clock,
    DutyCycleController &duty,
    PacketHandler &packetHandler,
    TdmaRadioService &radio,
    ISensor **sensors,
    size_t sensorCount,
    BatteryMonitor *battery)
    : _cfg(cfg),
      _clock(clock),
      _duty(duty),
      _packetHandler(packetHandler),
      _radio(radio),
      _sensors(sensors),
      _sensorCount(sensorCount),
      _battery(battery) {}

bool SmartFiresNodeApp::begin() {
    if (_cfg.enableBattery && _battery) {
        if (!_battery->begin()) {
            Serial.println("[App] Battery begin failed");
            return false;
        }
    }

    if (!_duty.begin()) {
        Serial.println("[App] DutyCycle begin failed");
        return false;
    }

    if (!_radio.begin()) {
        Serial.println("[App] Radio begin failed");
        return false;
    }

    _initialized = true;
    return true;
}

void SmartFiresNodeApp::update() {
    if (!_initialized) return;

    _radio.update();
    _duty.update();

    if (_cfg.enableBattery && _battery && _battery->ready()) {
        _battery->sample();
    }

    if (_duty.telemetryReady()) {
        const SensorSnapshot snap = buildSnapshot();
        _packetHandler.push(snap);

        if (_packetHandler.gpsPacketReady()) {
            uint8_t buf[BinaryPacket::kGpsLoRaSize];
            const uint8_t len = _packetHandler.takeGpsPacket(buf, sizeof(buf));
            if (len > 0) {
                _radio.enqueueTelemetry(buf, len);
            }
        }

        if (_packetHandler.bundleReady()) {
            uint8_t buf[BinaryPacket::kMaxBundleLoRaSize];
            const uint8_t len = _packetHandler.takeBundle(buf, sizeof(buf));
            if (len > 0) {
                _radio.enqueueTelemetry(buf, len);
            }
        }

        _duty.markTelemetrySent();
    }
}

SensorSnapshot SmartFiresNodeApp::buildSnapshot() const {
    SensorSnapshot snap;
    snap.sessionTimeMs = _clock.millis();
    snap.uptimeMs      = _clock.millis();

    for (size_t i = 0; i < _sensorCount; ++i) {
        if (_sensors[i]) {
            _sensors[i]->fillSnapshot(snap);
        }
    }

    return snap;
}
