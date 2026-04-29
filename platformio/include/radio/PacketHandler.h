#pragma once

#include "telemetry/BinaryPacket.h"
#include "telemetry/SensorSnapshot.h"

#include <stddef.h>
#include <stdint.h>

// Accumulates SensorSnapshot readings into PKT_BUNDLE and PKT_GPS LoRa payloads.
//
// Bundle path:
//   Call push() on each sensing cycle. When a reference + maxDeltas samples have
//   accumulated, the bundle is encoded and bundleReady() returns true. Retrieve
//   with takeBundle(), then accumulation continues into the next bundle.
//
// GPS path:
//   The first push() that carries a valid GPS fix (sensorFlags & GPS_FLAG) encodes
//   a one-shot PKT_GPS payload. gpsPacketReady() returns true; retrieve with
//   takeGpsPacket(). Stays suppressed until resetGpsSession() is called (call this
//   when a new session_id arrives from TIME_SYNC so the receiver gets a fresh fix).
//
// Output is raw LoRa payload bytes — pass directly to
// TdmaRadioService::enqueueTelemetry().

class PacketHandler {
public:
    static constexpr uint16_t GPS_FLAG = 0x04;

    struct Config {
        uint8_t nodeId    = 1;
        uint8_t maxDeltas = BinaryPacket::kBundleMaxDeltas;

        static Config make(uint8_t nodeId_ = 1,
                           uint8_t maxDeltas_ = BinaryPacket::kBundleMaxDeltas) {
            Config c;
            c.nodeId    = nodeId_;
            c.maxDeltas = maxDeltas_;
            return c;
        }
    };

    explicit PacketHandler(const Config &cfg);

    // Add one sensor reading.
    // Returns true if a complete bundle is now ready (check bundleReady()).
    // Also sets gpsPacketReady() on first valid GPS fix in this session.
    bool push(const SensorSnapshot &snap);

    // --- bundle ---
    bool    bundleReady() const;
    uint8_t takeBundle(uint8_t *buf, size_t bufSize);

    // --- GPS ---
    bool    gpsPacketReady() const;
    uint8_t takeGpsPacket(uint8_t *buf, size_t bufSize);

    // Call when a new session_id arrives from TIME_SYNC so GPS is re-sent.
    void resetGpsSession();

    // Full reset (new node session / reboot).
    void reset();

private:
    Config  _cfg;
    uint8_t _seq = 0;

    // Bundle state
    BinaryPacket::FullStatePayload _ref    = {};
    BinaryPacket::DeltaPayload     _deltas[BinaryPacket::kBundleMaxDeltas] = {};
    uint8_t _deltaCount  = 0;
    bool    _hasRef      = false;
    bool    _bundleReady = false;
    uint8_t _bundleBuf[BinaryPacket::kMaxBundleLoRaSize] = {};
    uint8_t _bundleLen   = 0;

    // GPS state
    bool    _gpsSent     = false;
    bool    _gpsReady    = false;
    uint8_t _gpsBuf[BinaryPacket::kGpsLoRaSize] = {};
    uint8_t _gpsLen      = 0;

    static BinaryPacket::FullStatePayload quantize(const SensorSnapshot &snap);
    static BinaryPacket::DeltaPayload     makeDelta(
        const BinaryPacket::FullStatePayload &ref,
        const BinaryPacket::FullStatePayload &sample);

    void tryEncodeGps(const SensorSnapshot &snap);
};
