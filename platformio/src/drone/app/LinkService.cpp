#include "LinkService.h"
#include "shared/BinaryPacket.h"

void LinkService::update() {
  _ctx.bridge.update();

  if (_ctx.bridge.hasError()) {
    Serial.print("[UART ERR] ");
    Serial.println(_ctx.bridge.lastError());
  }

  if (_ctx.bridge.bootSeen() && !_state.link.bootMessagePrinted) {
    _state.link.bootMessagePrinted = true;
  }

  if (_ctx.bridge.hasAck()) {
    const uint32_t ackSeq = _ctx.bridge.lastAckSeq();
    _state.link.lastAckedSeq = ackSeq;

    if (_state.link.waitingForAck &&
        ((ackSeq & 0xFF) == (_state.link.lastSentSeq & 0xFF))) {
      _state.link.waitingForAck = false;
    }
  }

  if (_ctx.bridge.hasRx()) {
    static char lastRxPrinted[96] = {0};
    if (strcmp(lastRxPrinted, _ctx.bridge.lastRx()) != 0) {
      strncpy(lastRxPrinted, _ctx.bridge.lastRx(), sizeof(lastRxPrinted) - 1);
      lastRxPrinted[sizeof(lastRxPrinted) - 1] = '\0';
    }
  }

  // TIME_SYNC from Feather: update session_time offset.
  if (_ctx.bridge.hasTimeSync()) {
    const uint32_t sid       = _ctx.bridge.lastSessionId();
    const uint32_t sessionMs = _ctx.bridge.lastSessionTimeMs();

    if (!_hasSynced || sid != _lastSyncSessionId) {
      // New Jetson session — reset offset tracking.
      _lastSyncSessionId = sid;
      _hasSynced         = true;
      Serial.print("[SYNC] new session id=");
      Serial.println(sid);
    }

    _sessionTimeOffset = static_cast<int64_t>(sessionMs) - static_cast<int64_t>(_clock.millis());
    _ctx.bridge.clearTimeSync();

    Serial.print("[SYNC] offset=");
    Serial.print(static_cast<int32_t>(_sessionTimeOffset));
    Serial.println("ms");
  }
}

void LinkService::maybeSendTelemetry(uint8_t nodeId) {
  const uint32_t now = _clock.millis();

  // if (!_state.sensingEnabled || _state.sensorsSleeping || _state.wakeupSequenceActive) return;
  if (now - _state.link.lastSendMs < UartLoRaBridge::kTelemetryPeriodMs) return;

  _state.link.lastSendMs = now;

  if (_state.link.waitingForAck) {
    // Serial.println("[UART] still waiting for ACK, skipping send");
    return;
  }

  ++_state.link.seq;
  sendTelemetryFrame(nodeId, _state.link.seq);
  _state.link.lastSentSeq = _state.link.seq;
  _state.link.lastSendTimeMs = now;
  _state.link.waitingForAck = true;
}

void LinkService::handleAckTimeout() {
  const uint32_t now = _clock.millis();

  if (_state.link.waitingForAck &&
      (now - _state.link.lastSendTimeMs >= UartLoRaBridge::kAckTimeoutMs)) {
    // Serial.print("[UART] ACK timeout for seq ");
    // Serial.println(_state.link.lastSentSeq);
    _state.link.waitingForAck = false;
  }
}

void LinkService::sendTelemetryFrame(uint8_t nodeId, uint32_t seq) {
  const uint32_t now = _clock.millis();
  TelemetryPacket pkt = _telemetry.build(seq, now);

  BinaryPacket::FullStatePayload payload{};
  // session_time uses Jetson-synced offset when available; falls back to local millis().
  payload.session_time   = static_cast<uint32_t>(static_cast<int64_t>(now) + _sessionTimeOffset);
  payload.uptime_ms      = pkt.uptimeMs;
  payload.sensor_flags   = pkt.sensorFlags;
  payload.wind_cms       = static_cast<uint16_t>(pkt.windMps * 100.0f + 0.5f);
  payload.temp_cdegc     = static_cast<int16_t>(pkt.tempC * 100.0f);
  payload.humidity_cpct  = static_cast<uint16_t>(pkt.humidityPct * 100.0f + 0.5f);
  payload.pm1_0_ug10     = static_cast<uint16_t>(isnan(pkt.pm1_0) ? 0u : (uint16_t)(pkt.pm1_0 * 10.0f + 0.5f));
  payload.pm2_5_ug10     = static_cast<uint16_t>(isnan(pkt.pm2_5) ? 0u : (uint16_t)(pkt.pm2_5 * 10.0f + 0.5f));
  payload.pm4_0_ug10     = static_cast<uint16_t>(isnan(pkt.pm4_0) ? 0u : (uint16_t)(pkt.pm4_0 * 10.0f + 0.5f));
  payload.pm10_ug10      = static_cast<uint16_t>(isnan(pkt.pm10)  ? 0u : (uint16_t)(pkt.pm10  * 10.0f + 0.5f));
  payload.lat_e7         = static_cast<int32_t>(pkt.lat * 1e7);
  payload.lon_e7         = static_cast<int32_t>(pkt.lon * 1e7);

  uint8_t frame[48];
  const size_t len = BinaryPacket::encodeFullStateFrame(
      nodeId,
      static_cast<uint8_t>(seq & 0xFF),
      payload,
      frame,
      sizeof(frame));

  if (len > 0) {
    _ctx.bridge.sendBinaryFrame(frame, len);
  } else {
    Serial.println("[UART TX] encode failed");
  }
}
