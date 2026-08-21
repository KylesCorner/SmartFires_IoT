// ---
// description: Accumulates SensorSnapshot readings into PKT_BUNDLE and PKT_STATUS LoRa payloads.
// role: implementation
// ---
#pragma once

#include "config/NetworkConfig.h"
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

    // --- Timed duty-cycle windows ---
    //
    // Window edges are announced on the wire by PKT_WINDOW_BEGIN/PKT_WINDOW_END
    // (SmartFiresNodeApp owns those), not by flags on the sample stream, so the
    // handler's only remaining window duty is to say whether the accumulator is
    // mid-bundle.
    //
    // hasPartialBundle() drives DutyCycleController's active-window hold: the
    // window runs past activeSampleMs until this goes false, so every bundle on
    // the air carries a full maxDeltas+1 samples and no runt is ever encoded. A
    // runt spends a fresh 20-byte FullState reference on a handful of samples,
    // and — before the hold existed — was also the frame that could not be acked
    // before standby.
    //
    // flushWindow() is the escape hatch for when the hold hits its overrun cap
    // (a wedged sensor starving the sample tick). It force-encodes whatever has
    // accumulated so those samples are not lost, and returns true if a bundle is
    // now ready to take. In normal operation the hold means nothing is
    // accumulated at window close and this returns false.
    bool hasPartialBundle() const;
    bool flushWindow();

    // --- STATUS (GPS + battery, interval from Config::statusIntervalMs) ---
    bool    statusPacketReady() const;
    uint8_t takeStatusPacket(uint8_t *buf, size_t bufSize);

    // Force STATUS to be sent on the next push (call on new session_id from TIME_SYNC).
    void resetStatusTimer();
    void setNodeId(uint8_t nodeId);
    void setBundleEncodingEnabled(bool enabled);
    void setLinkStats(uint32_t retxTotal, uint32_t failTotal);

    // Radio TX power state to report in the next STATUS. Pushed in by
    // SmartFiresNodeApp (from TdmaRadioService::txPowerDbm() and its own mode
    // flag) rather than pulled, so the handler keeps no radio dependency —
    // same shape as setLinkStats() above. See StatusPayload::tx_power_dbm for
    // why this is sourced from the node's own radio and not from the base's
    // record of what it commanded.
    //
    // Both values travel together because they are read together: reporting a
    // level without the mode that produced it leaves the dashboard unable to
    // tell an operator-pinned 7 dBm from one the control loop chose.
    void setTxPowerState(int8_t dbm, bool isStatic);

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
    int8_t   _txPowerDbm = NetworkConfig::kRadioTxPowerDbm;
    bool     _txPowerStatic = false;

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
    bool encodeAccumulatedBundle();
    void tryEncodeStatus(const SensorSnapshot &snap);
};
