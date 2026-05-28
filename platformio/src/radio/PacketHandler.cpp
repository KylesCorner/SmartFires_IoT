#include "radio/PacketHandler.h"

#include "logging/DebugLogger.h"

#include <limits.h>
#include <math.h>
#include <string.h>

namespace {

inline int32_t roundDivideSigned(int32_t value, int32_t divisor) {
    if (value >= 0) {
        return (value + divisor / 2) / divisor;
    }
    return -(((-value) + divisor / 2) / divisor);
}

inline int8_t clampToInt8(int32_t value, uint8_t flagBit, uint8_t &flags) {
    if (value < INT8_MIN) {
        flags |= flagBit;
        return INT8_MIN;
    }
    if (value > INT8_MAX) {
        flags |= flagBit;
        return INT8_MAX;
    }
    return static_cast<int8_t>(value);
}

inline int16_t clampToInt16(int32_t value, uint8_t flagBit, uint8_t &flags) {
    if (value < INT16_MIN) {
        flags |= flagBit;
        return INT16_MIN;
    }
    if (value > INT16_MAX) {
        flags |= flagBit;
        return INT16_MAX;
    }
    return static_cast<int16_t>(value);
}

} // namespace

PacketHandler::PacketHandler(const Config &cfg) : _cfg(cfg) {}

bool PacketHandler::push(const SensorSnapshot &snap) {
    _bundleReady = false;

    LOG_DEBUG("packet",
              "push_start session_ms=%lu sensor_flags=0x%04X has_ref=%u delta_count=%u",
              static_cast<unsigned long>(snap.sessionTimeMs),
              static_cast<unsigned int>(snap.sensorFlags),
              _hasRef ? 1 : 0,
              static_cast<unsigned int>(_deltaCount));

    tryEncodeStatus(snap);

    if (!_bundleEncodingEnabled) {
        return false;
    }

    const BinaryPacket::FullStatePayload sample = quantize(snap);

    if (!_hasRef) {
        _ref        = sample;
        _hasRef     = true;
        _deltaCount = 0;

        LOG_DEBUG("packet", "bundle_ref_set session_ms=%lu",
                  static_cast<unsigned long>(snap.sessionTimeMs));

        return false;
    }

    _deltas[_deltaCount++] = makeDelta(_ref, sample);

    LOG_DEBUG("packet", "bundle_delta_added count=%u/%u",
              static_cast<unsigned int>(_deltaCount),
              static_cast<unsigned int>(_cfg.maxDeltas));

    if (_deltaCount < _cfg.maxDeltas) {
        return false;
    }

    _bundleLen = BinaryPacket::encodeBundlePayload(
        _cfg.nodeId, _seq++,
        _ref, _deltas, _deltaCount,
        _bundleBuf, sizeof(_bundleBuf));

    _bundleReady = (_bundleLen > 0);
    _hasRef      = false;
    _deltaCount  = 0;

    if (_bundleReady) {
        LOG_INFO("packet", "bundle_encoded len=%u node=%u",
                 static_cast<unsigned int>(_bundleLen),
                 static_cast<unsigned int>(_cfg.nodeId));
    } else {
        LOG_WARN("packet", "bundle_encode_failed node=%u",
                 static_cast<unsigned int>(_cfg.nodeId));
    }

    return _bundleReady;
}

// --- bundle ---

bool PacketHandler::bundleReady() const {
    return _bundleReady;
}

uint8_t PacketHandler::takeBundle(uint8_t *buf, size_t bufSize) {
    if (!_bundleReady || !buf || bufSize < _bundleLen) return 0;
    memcpy(buf, _bundleBuf, _bundleLen);
    _bundleReady = false;
    return _bundleLen;
}

// --- STATUS ---

bool PacketHandler::statusPacketReady() const {
    return _statusReady;
}

uint8_t PacketHandler::takeStatusPacket(uint8_t *buf, size_t bufSize) {
    if (!_statusReady || !buf || bufSize < _statusLen) return 0;
    memcpy(buf, _statusBuf, _statusLen);
    _statusReady = false;
    return _statusLen;
}

void PacketHandler::resetStatusTimer() {
    _statusEverSent = false;
    _statusReady    = false;
    _statusLen      = 0;
}

void PacketHandler::setNodeId(uint8_t nodeId) {
    _cfg.nodeId = nodeId;
}

void PacketHandler::setBundleEncodingEnabled(bool enabled) {
    if (_bundleEncodingEnabled == enabled) {
        return;
    }

    _bundleEncodingEnabled = enabled;

    if (!enabled) {
        resetBundleState();
    }

    LOG_INFO("packet", "bundle_encoding=%s",
             _bundleEncodingEnabled ? "ON" : "OFF");
}

// --- full reset ---

void PacketHandler::reset() {
    _seq         = 0;
    resetBundleState();
    resetStatusTimer();
    memset(_statusBuf, 0, sizeof(_statusBuf));
}

// ---------- private ----------

void PacketHandler::resetBundleState() {
    _hasRef      = false;
    _deltaCount  = 0;
    _bundleReady = false;
    _bundleLen   = 0;
    memset(&_ref, 0, sizeof(_ref));
    memset(_deltas, 0, sizeof(_deltas));
    memset(_bundleBuf, 0, sizeof(_bundleBuf));
}

void PacketHandler::tryEncodeStatus(const SensorSnapshot &snap) {
    const bool intervalElapsed =
        !_statusEverSent ||
        (snap.sessionTimeMs - _lastStatusMs >= _cfg.statusIntervalMs);

    if (!intervalElapsed) return;

    BinaryPacket::StatusPayload sp = {};
    uint8_t flags = 0;

    if (snap.sensorFlags & GPS_FLAG) {
        sp.lat_e7 = static_cast<int32_t>(snap.latDeg * 1e7f);
        sp.lon_e7 = static_cast<int32_t>(snap.lonDeg * 1e7f);
        flags |= BinaryPacket::STATUS_GPS_VALID;
    }

    if (snap.batteryValid) {
        sp.battery_mv  = snap.batteryMv;
        sp.battery_pct = snap.batteryPct;
        flags |= BinaryPacket::STATUS_BATT_VALID;
    }

    if (snap.imuValid) {
        sp.heading_deg_x10  = static_cast<uint16_t>(snap.headingDeg * 10.0f + 0.5f);
        sp.heading_accuracy = snap.headingAccuracy;
        flags |= BinaryPacket::STATUS_IMU_VALID;
    }

    sp.flags = flags;

    _statusLen   = BinaryPacket::encodeStatusPayload(
        _cfg.nodeId, _seq++, sp, _statusBuf, sizeof(_statusBuf));
    _statusReady = (_statusLen > 0);

    if (_statusReady) {
        _lastStatusMs    = snap.sessionTimeMs;
        _statusEverSent  = true;

        LOG_INFO("packet",
                 "status_encoded len=%u node=%u flags=0x%02X session_ms=%lu",
                 static_cast<unsigned int>(_statusLen),
                 static_cast<unsigned int>(_cfg.nodeId),
                 static_cast<unsigned int>(sp.flags),
                 static_cast<unsigned long>(snap.sessionTimeMs));
        if ((sp.flags & BinaryPacket::STATUS_IMU_VALID) != 0u) {
            LOG_DEBUG("packet",
                      "status_imu_payload node=%u heading_deg=%.1f accuracy_deg=%.2f",
                      static_cast<unsigned int>(_cfg.nodeId),
                      static_cast<float>(sp.heading_deg_x10) / 10.0f,
                      static_cast<float>(sp.heading_accuracy) / 4096.0f);
        }
    } else {
        LOG_WARN("packet", "status_encode_failed node=%u flags=0x%02X",
                 static_cast<unsigned int>(_cfg.nodeId),
                 static_cast<unsigned int>(sp.flags));
    }
}

BinaryPacket::FullStatePayload PacketHandler::quantize(const SensorSnapshot &snap) {
    BinaryPacket::FullStatePayload p = {};
    p.session_time  = snap.sessionTimeMs;
    p.sensor_flags  = snap.sensorFlags;
    p.wind_cms      = static_cast<uint16_t>(snap.windMps     * 100.0f + 0.5f);
    p.temp_cdegc    = static_cast<int16_t> (snap.tempC       * 100.0f);
    p.humidity_cpct = static_cast<uint16_t>(snap.humidityPct * 100.0f + 0.5f);
    p.pm1_0_ug10    = static_cast<uint16_t>(snap.pm1_0       * 10.0f  + 0.5f);
    p.pm2_5_ug10    = static_cast<uint16_t>(snap.pm2_5       * 10.0f  + 0.5f);
    p.pm4_0_ug10    = static_cast<uint16_t>(snap.pm4_0       * 10.0f  + 0.5f);
    p.pm10_ug10     = static_cast<uint16_t>(snap.pm10        * 10.0f  + 0.5f);
    return p;
}

BinaryPacket::DeltaPayload PacketHandler::makeDelta(
    const BinaryPacket::FullStatePayload &ref,
    const BinaryPacket::FullStatePayload &s)
{
    BinaryPacket::DeltaPayload d = {};

    const uint32_t dtMs = static_cast<uint32_t>(s.session_time - ref.session_time);
    uint8_t flags = 0;

    uint32_t dtTicks = (dtMs + 125u) / 250u;
    if (dtTicks > 255u) {
        dtTicks = 255u;
        flags |= BinaryPacket::DELTA_FLAG_DT_CLAMPED;
    }

    d.dt_ticks_250ms      = static_cast<uint8_t>(dtTicks);
    d.wind_cms            = s.wind_cms;

    const int32_t tempDeltaCdeg = static_cast<int32_t>(s.temp_cdegc) - static_cast<int32_t>(ref.temp_cdegc);
    d.temp_delta_deci_c = clampToInt8(
        roundDivideSigned(tempDeltaCdeg, 10), BinaryPacket::DELTA_FLAG_TEMP_CLAMPED, flags);

    const int32_t humidityDeltaCpct =
        static_cast<int32_t>(s.humidity_cpct) - static_cast<int32_t>(ref.humidity_cpct);
    d.humidity_delta_0p2pct = clampToInt8(
        roundDivideSigned(humidityDeltaCpct, 20), BinaryPacket::DELTA_FLAG_HUMID_CLAMPED, flags);

    const int32_t pm1DeltaUg10 = static_cast<int32_t>(s.pm1_0_ug10) - static_cast<int32_t>(ref.pm1_0_ug10);
    d.pm1_0_delta_ug = clampToInt8(
        roundDivideSigned(pm1DeltaUg10, 10), BinaryPacket::DELTA_FLAG_PM1_CLAMPED, flags);

    const int32_t pm25DeltaUg10 = static_cast<int32_t>(s.pm2_5_ug10) - static_cast<int32_t>(ref.pm2_5_ug10);
    d.pm2_5_delta_ug10 = clampToInt16(pm25DeltaUg10, BinaryPacket::DELTA_FLAG_PM2_5_CLAMPED, flags);

    const int32_t pm4DeltaUg10 = static_cast<int32_t>(s.pm4_0_ug10) - static_cast<int32_t>(ref.pm4_0_ug10);
    d.pm4_0_delta_ug = clampToInt8(
        roundDivideSigned(pm4DeltaUg10, 10), BinaryPacket::DELTA_FLAG_PM4_CLAMPED, flags);

    const int32_t pm10DeltaUg10 = static_cast<int32_t>(s.pm10_ug10) - static_cast<int32_t>(ref.pm10_ug10);
    d.pm10_delta_ug10 = clampToInt16(pm10DeltaUg10, BinaryPacket::DELTA_FLAG_PM10_CLAMPED, flags);

    d.flags = flags;
    return d;
}
