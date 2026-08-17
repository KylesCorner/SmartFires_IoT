// ---
// description: Accumulates SensorSnapshot readings into PKT_BUNDLE and PKT_STATUS LoRa payloads.
// role: implementation
// ---
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
    static constexpr uint16_t WIND_FLAG  = 0x01;
    static constexpr uint16_t SHT31_FLAG = 0x02;
    static constexpr uint16_t GPS_FLAG   = 0x04;
    static constexpr uint16_t SPS30_FLAG = 0x10;

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

    // --- Timed duty-cycle windows (PktHeader::flags) ---
    //
    // beginWindow() marks the next bundle to be encoded as the first of a new
    // active window. flushWindow() closes the window: it force-encodes whatever
    // partial bundle has accumulated (which would otherwise sit unsent across
    // the MCU standby) and marks it as the window's last. Returns true if a
    // bundle is now ready to take.
    //
    // A window short enough to produce a single bundle gets both bits on it.
    // A window whose samples happened to land exactly on a bundle boundary has
    // nothing left to flush, so no packet carries WINDOW_LAST — a receiver
    // should treat the next WINDOW_FIRST as an implicit window close.
    void beginWindow();
    bool flushWindow();

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
    BinaryPacket::FullStatePayload _ref        = {};
    BinaryPacket::FullStatePayload _prevSample = {};
    BinaryPacket::DeltaPayload     _deltas[BinaryPacket::kBundleMaxDeltas] = {};
    uint8_t _deltaCount  = 0;
    bool    _hasRef      = false;
    bool    _bundleReady = false;
    // PktHeader::flags bits to stamp on the next bundle encode, cleared once
    // one actually reaches a frame.
    uint8_t _pendingBundleFlags = 0;
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

    // Last-good values substituted when a sensor's validity flag is clear, so
    // SensorSnapshot's -1.0f placeholders never reach the wire (they alias real
    // readings and, on a bundle reference, poison every delta via the int8 clamp).
    float _lastGoodWindMps     = 0.0f;
    float _lastGoodTempC       = 0.0f;
    float _lastGoodHumidityPct = 0.0f;
    float _lastGoodPm1_0       = 0.0f;
    float _lastGoodPm2_5       = 0.0f;
    float _lastGoodPm4_0       = 0.0f;
    float _lastGoodPm10        = 0.0f;

    SensorSnapshot gateInvalidSensors(const SensorSnapshot &snap);

    static BinaryPacket::FullStatePayload quantize(const SensorSnapshot &snap);
    static BinaryPacket::DeltaPayload     makeDelta(
        const BinaryPacket::FullStatePayload &ref,
        const BinaryPacket::FullStatePayload &prev,
        const BinaryPacket::FullStatePayload &sample);

    void resetBundleState();
    bool encodeAccumulatedBundle(uint8_t extraFlags);
    void tryEncodeStatus(const SensorSnapshot &snap);
};
