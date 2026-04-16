#include "LinkService.h"
#include "shared/BinaryPacket.h"

void LinkService::update() {
  _ctx.bridge.update();

  if (_ctx.bridge.hasError()) {
    Serial.print("[UART ERR] ");
    Serial.println(_ctx.bridge.lastError());
  }

  if (_ctx.bridge.bootSeen() && !_state.link.bootMessagePrinted) {
    Serial.println("[UART] Feather boot seen");
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
      Serial.print("[UART RX PAYLOAD] ");
      Serial.println(_ctx.bridge.lastRx());
      strncpy(lastRxPrinted, _ctx.bridge.lastRx(), sizeof(lastRxPrinted) - 1);
      lastRxPrinted[sizeof(lastRxPrinted) - 1] = '\0';
    }
  }
}

void LinkService::maybeSendTelemetry(uint8_t nodeId) {
  const uint32_t now = _clock.millis();

  if (!_state.sensingEnabled) return;
  if (now - _state.link.lastSendMs < UartLoRaBridge::kTelemetryPeriodMs) return;

  _state.link.lastSendMs = now;

  if (_state.link.waitingForAck) {
    Serial.println("[UART] still waiting for ACK, skipping send");
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
    Serial.print("[UART] ACK timeout for seq ");
    Serial.println(_state.link.lastSentSeq);
    _state.link.waitingForAck = false;
  }
}

void LinkService::sendTelemetryFrame(uint8_t nodeId, uint32_t seq) {
  const uint32_t now = _clock.millis();
  TelemetryPacket pkt = _telemetry.build(seq, now);

  BinaryPacket::FullStatePayload payload{};
  payload.session_time = pkt.uptimeMs;
  payload.uptime_ms = pkt.uptimeMs;
  payload.sensor_flags = pkt.sensorFlags;
  payload.flame = pkt.flameDetected ? 1u : 0u;
  payload.wind_cms = static_cast<uint16_t>(pkt.windMps * 100.0f + 0.5f);
  payload.temp_cdegc = static_cast<int16_t>(pkt.tempC * 100.0f);
  payload.humidity_cpct = static_cast<uint16_t>(pkt.humidityPct * 100.0f + 0.5f);
  payload.lidar_cm = static_cast<uint16_t>(pkt.lidarCm > 0 ? pkt.lidarCm : 0);
  payload.lat_e7 = static_cast<int32_t>(pkt.lat * 1e7);
  payload.lon_e7 = static_cast<int32_t>(pkt.lon * 1e7);

  uint8_t frame[40];
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
