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
      if (_waitingForGpsAck) {
        _waitingForGpsAck   = false;
        _gpsSentThisSession = true;
        Serial.println("[GPS TX] ACKed — GPS sent for this session");
      }
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
      _lastSyncSessionId      = sid;
      _hasSynced              = true;
      _gpsSentThisSession     = false;  // new session — allow GPS to be re-sent
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

  // Accumulate one sample per telemetry period.
  if (now - _state.link.lastSendMs < UartLoRaBridge::kTelemetryPeriodMs) return;
  _state.link.lastSendMs = now;

  if (_sampleCount < kBundleSamples) {
    _sampleBuf[_sampleCount++] = buildPayload(now);
  }

  // Send when the buffer is full and the Feather is ready.
  if (_sampleCount < kBundleSamples) return;
  if (_state.link.waitingForAck)     return;

  ++_state.link.seq;
  sendBundleFrame(nodeId, _state.link.seq);
  _state.link.lastSentSeq    = _state.link.seq;
  _state.link.lastSendTimeMs = now;
  _state.link.waitingForAck  = true;
  _sampleCount = 0;
}

void LinkService::handleAckTimeout() {
  const uint32_t now = _clock.millis();

  if (!_state.link.waitingForAck) return;
  if (now - _state.link.lastSendTimeMs < UartLoRaBridge::kAckTimeoutMs) return;

  if (_waitingForGpsAck) {
    if (_gpsRetryCount < kGpsMaxRetries) {
      ++_gpsRetryCount;
      Serial.print("[GPS TX] no ACK — retry ");
      Serial.println(_gpsRetryCount);
      sendGpsFrame();
    } else {
      // All retries exhausted — release lock so bundles can proceed.
      // Leave _gpsSentThisSession = false so maybeSendGpsOnce() will try again later.
      _state.link.waitingForAck = false;
      _waitingForGpsAck         = false;
    }
  } else {
    _state.link.waitingForAck = false;
  }
}

BinaryPacket::FullStatePayload LinkService::buildPayload(uint32_t now) const {
  TelemetryPacket pkt = _telemetry.build(0, now);

  BinaryPacket::FullStatePayload p{};
  p.session_time   = static_cast<uint32_t>(static_cast<int64_t>(now) + _sessionTimeOffset);
  p.uptime_ms      = pkt.uptimeMs;
  p.sensor_flags   = pkt.sensorFlags;
  p.wind_cms       = static_cast<uint16_t>(pkt.windMps * 100.0f + 0.5f);
  p.temp_cdegc     = static_cast<int16_t>(pkt.tempC * 100.0f);
  p.humidity_cpct  = static_cast<uint16_t>(pkt.humidityPct * 100.0f + 0.5f);
  p.pm1_0_ug10     = static_cast<uint16_t>(isnan(pkt.pm1_0) ? 0u : (uint16_t)(pkt.pm1_0 * 10.0f + 0.5f));
  p.pm2_5_ug10     = static_cast<uint16_t>(isnan(pkt.pm2_5) ? 0u : (uint16_t)(pkt.pm2_5 * 10.0f + 0.5f));
  p.pm4_0_ug10     = static_cast<uint16_t>(isnan(pkt.pm4_0) ? 0u : (uint16_t)(pkt.pm4_0 * 10.0f + 0.5f));
  p.pm10_ug10      = static_cast<uint16_t>(isnan(pkt.pm10)  ? 0u : (uint16_t)(pkt.pm10  * 10.0f + 0.5f));
  return p;
}

void LinkService::maybeSendGpsOnce(uint8_t nodeId) {
  if (_gpsSentThisSession) return;
  if (_waitingForGpsAck) return;       // retry handled by handleAckTimeout
  if (_state.link.waitingForAck) return; // let the in-flight bundle complete first

  TelemetryPacket pkt = _telemetry.build(0, _clock.millis());
  if (!(pkt.sensorFlags & TelemetryService::TF_GPS)) return;  // no valid fix yet

  _gpsPending.lat_e7 = static_cast<int32_t>(pkt.lat * 1e7);
  _gpsPending.lon_e7 = static_cast<int32_t>(pkt.lon * 1e7);
  _gpsNodeId         = nodeId;
  _gpsRetryCount     = 0;

  Serial.print("[GPS TX] lat=");
  Serial.print(pkt.lat, 7);
  Serial.print(" lon=");
  Serial.println(pkt.lon, 7);

  sendGpsFrame();
}

void LinkService::sendGpsFrame() {
  ++_state.link.seq;
  uint8_t frame[20];
  const size_t len = BinaryPacket::encodeGpsFrame(
      _gpsNodeId,
      static_cast<uint8_t>(_state.link.seq & 0xFF),
      _gpsPending,
      frame, sizeof(frame));
  if (len > 0) {
    _ctx.bridge.sendBinaryFrame(frame, len);
    _state.link.lastSentSeq    = _state.link.seq;
    _state.link.lastSendTimeMs = _clock.millis();
    _state.link.waitingForAck  = true;
    _waitingForGpsAck          = true;
  }
}

void LinkService::sendBundleFrame(uint8_t nodeId, uint32_t seq) {
  const BinaryPacket::FullStatePayload& ref = _sampleBuf[0];
  const uint8_t n = static_cast<uint8_t>(_sampleCount - 1);

  BinaryPacket::DeltaPayload deltas[BinaryPacket::kBundleMaxDeltas];
  for (uint8_t i = 0; i < n; ++i) {
    const BinaryPacket::FullStatePayload& s = _sampleBuf[i + 1];
    BinaryPacket::DeltaPayload& d = deltas[i];
    d.dt_ms               = static_cast<uint16_t>(s.session_time - ref.session_time);
    d.wind_cms            = s.wind_cms;
    d.temp_delta_cdegc    = static_cast<int16_t>(
                              static_cast<int32_t>(s.temp_cdegc) - static_cast<int32_t>(ref.temp_cdegc));
    d.humidity_delta_cpct = static_cast<int16_t>(
                              static_cast<int32_t>(s.humidity_cpct) - static_cast<int32_t>(ref.humidity_cpct));
    d.pm1_0_delta         = static_cast<int16_t>(
                              static_cast<int32_t>(s.pm1_0_ug10) - static_cast<int32_t>(ref.pm1_0_ug10));
    d.pm2_5_delta         = static_cast<int16_t>(
                              static_cast<int32_t>(s.pm2_5_ug10) - static_cast<int32_t>(ref.pm2_5_ug10));
    d.pm4_0_delta         = static_cast<int16_t>(
                              static_cast<int32_t>(s.pm4_0_ug10) - static_cast<int32_t>(ref.pm4_0_ug10));
    d.pm10_delta          = static_cast<int16_t>(
                              static_cast<int32_t>(s.pm10_ug10) - static_cast<int32_t>(ref.pm10_ug10));
  }

  uint8_t frame[200];
  const size_t len = BinaryPacket::encodeBundleFrame(
      nodeId,
      static_cast<uint8_t>(seq & 0xFF),
      ref, deltas, n,
      frame, sizeof(frame));

  if (len > 0) {
    _ctx.bridge.sendBinaryFrame(frame, len);
    Serial.print("[UART TX] bundle seq=");
    Serial.print(seq & 0xFF);
    Serial.print(" deltas=");
    Serial.print(n);
    Serial.print(" len=");
    Serial.println(len);
  } else {
    Serial.println("[UART TX] bundle encode failed");
  }
}
