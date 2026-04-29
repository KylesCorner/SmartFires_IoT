#include "radio/PacketHandler.h"

#include <math.h>
#include <string.h>

PacketHandler::PacketHandler(const Config &cfg) : _cfg(cfg) {}

bool PacketHandler::push(const SensorSnapshot &snap) {
    _bundleReady = false;

    tryEncodeGps(snap);

    const BinaryPacket::FullStatePayload sample = quantize(snap);

    if (!_hasRef) {
        _ref        = sample;
        _hasRef     = true;
        _deltaCount = 0;
        return false;
    }

    _deltas[_deltaCount++] = makeDelta(_ref, sample);

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

// --- GPS ---

bool PacketHandler::gpsPacketReady() const {
    return _gpsReady;
}

uint8_t PacketHandler::takeGpsPacket(uint8_t *buf, size_t bufSize) {
    if (!_gpsReady || !buf || bufSize < _gpsLen) return 0;
    memcpy(buf, _gpsBuf, _gpsLen);
    _gpsReady = false;
    return _gpsLen;
}

void PacketHandler::resetGpsSession() {
    _gpsSent  = false;
    _gpsReady = false;
    _gpsLen   = 0;
}

// --- full reset ---

void PacketHandler::reset() {
    _seq         = 0;
    _hasRef      = false;
    _deltaCount  = 0;
    _bundleReady = false;
    _bundleLen   = 0;
    memset(&_ref,      0, sizeof(_ref));
    memset(_deltas,    0, sizeof(_deltas));
    memset(_bundleBuf, 0, sizeof(_bundleBuf));
    resetGpsSession();
    memset(_gpsBuf, 0, sizeof(_gpsBuf));
}

// ---------- private ----------

void PacketHandler::tryEncodeGps(const SensorSnapshot &snap) {
    if (_gpsSent) return;
    if (!(snap.sensorFlags & GPS_FLAG)) return;

    BinaryPacket::GpsPayload gps;
    gps.lat_e7 = static_cast<int32_t>(snap.latDeg * 1e7f);
    gps.lon_e7 = static_cast<int32_t>(snap.lonDeg * 1e7f);

    _gpsLen   = BinaryPacket::encodeGpsPayload(_cfg.nodeId, _seq++, gps,
                                                _gpsBuf, sizeof(_gpsBuf));
    _gpsReady = (_gpsLen > 0);
    _gpsSent  = true;
}

BinaryPacket::FullStatePayload PacketHandler::quantize(const SensorSnapshot &snap) {
    BinaryPacket::FullStatePayload p = {};
    p.session_time  = snap.sessionTimeMs;
    p.uptime_ms     = snap.uptimeMs;
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
    d.dt_ms               = static_cast<uint16_t>(s.session_time - ref.session_time);
    d.wind_cms            = s.wind_cms;
    d.temp_delta_cdegc    = static_cast<int16_t>(s.temp_cdegc - ref.temp_cdegc);
    d.humidity_delta_cpct = static_cast<int16_t>(
        static_cast<int32_t>(s.humidity_cpct) - static_cast<int32_t>(ref.humidity_cpct));
    d.pm1_0_delta         = static_cast<int16_t>(
        static_cast<int32_t>(s.pm1_0_ug10) - static_cast<int32_t>(ref.pm1_0_ug10));
    d.pm2_5_delta         = static_cast<int16_t>(
        static_cast<int32_t>(s.pm2_5_ug10) - static_cast<int32_t>(ref.pm2_5_ug10));
    d.pm4_0_delta         = static_cast<int16_t>(
        static_cast<int32_t>(s.pm4_0_ug10) - static_cast<int32_t>(ref.pm4_0_ug10));
    d.pm10_delta          = static_cast<int16_t>(
        static_cast<int32_t>(s.pm10_ug10)  - static_cast<int32_t>(ref.pm10_ug10));
    return d;
}
