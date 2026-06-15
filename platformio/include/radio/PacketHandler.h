#pragma once

#include "telemetry/BinaryPacket.h"
#include "telemetry/SensorSnapshot.h"

#include <stddef.h>
#include <stdint.h>

// Accumulates SensorSnapshot readings into PKT_BUNDLE and PKT_STATUS LoRa payloads.
//
// Bundle path:
//   Call push() on each sensing cycle. When a reference + maxDeltas samples have
//   accumulated, the bundle is encoded and bundleReady() returns true. Retrieve
//   with takeBundle(), then accumulation continues into the next bundle.
//
// STATUS path:
//   A PKT_STATUS (GPS + battery) is emitted immediately on the first push(), then
//   every kStatusIntervalMs thereafter (keyed off snap.sessionTimeMs). Retrieve
//   with takeStatusPacket(). Call resetStatusTimer() when a new session_id arrives
//   from TIME_SYNC to force an immediate re-send.
//
// Output is raw LoRa payload bytes — pass directly to
// TdmaRadioService::enqueueTelemetry().

class PacketHandler {
public:
    static constexpr uint16_t GPS_FLAG  = 0x04;

    static constexpr uint32_t kStatusIntervalMs = 15u * 60u * 1000u;  // 15 min

    struct Config {
        uint8_t nodeId    = 1;
        uint8_t maxDeltas = BinaryPacket::kBundleMaxDeltas;
        uint32_t statusIntervalMs = kStatusIntervalMs;

        static Config make(uint8_t nodeId_ = 1,
                           uint8_t maxDeltas_ = BinaryPacket::kBundleMaxDeltas,
                           uint32_t statusIntervalMs_ = kStatusIntervalMs) {
            Config c;
            c.nodeId    = nodeId_;
            c.maxDeltas = maxDeltas_;
            c.statusIntervalMs = statusIntervalMs_;
            return c;
        }
    };

    explicit PacketHandler(const Config &cfg);

    // Add one sensor reading.
    // Returns true if a complete bundle is now ready (check bundleReady()).
    // Also sets statusPacketReady() on first push and every configured status interval after.
    bool push(const SensorSnapshot &snap);

    // --- bundle ---
    bool    bundleReady() const;
    uint8_t takeBundle(uint8_t *buf, size_t bufSize);

    // --- STATUS (GPS + battery, interval from Config::statusIntervalMs) ---
    bool    statusPacketReady() const;
    uint8_t takeStatusPacket(uint8_t *buf, size_t bufSize);

    // Force STATUS to be sent on the next push (call on new session_id from TIME_SYNC).
    void resetStatusTimer();
    void setNodeId(uint8_t nodeId);
    void setBundleEncodingEnabled(bool enabled);
    void setLinkStats(uint32_t retxTotal, uint32_t failTotal);

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

    // STATUS state
    bool     _statusEverSent    = false;
    uint32_t _lastStatusMs      = 0;
    bool     _statusReady       = false;
    uint8_t  _statusBuf[BinaryPacket::kStatusLoRaSize] = {};
    uint8_t  _statusLen         = 0;
    bool     _bundleEncodingEnabled = true;
    uint32_t _retxTotal  = 0;
    uint32_t _failTotal  = 0;

    static BinaryPacket::FullStatePayload quantize(const SensorSnapshot &snap);
    static BinaryPacket::DeltaPayload     makeDelta(
        const BinaryPacket::FullStatePayload &ref,
        const BinaryPacket::FullStatePayload &sample);

    void resetBundleState();
    void tryEncodeStatus(const SensorSnapshot &snap);
};
