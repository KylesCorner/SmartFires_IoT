#include "app/SmartFiresBaseApp.h"

#include <Arduino.h>

namespace {

const char *pktTypeName(uint8_t pktType) {
  switch (pktType) {
    case BinaryPacket::PKT_AWAKEN:
      return "AWAKEN";
    case BinaryPacket::PKT_BUNDLE:
      return "BUNDLE";
    case BinaryPacket::PKT_STATUS:
      return "STATUS";
    case BinaryPacket::PKT_FULL_STATE:
      return "FULL_STATE";
    case BinaryPacket::PKT_TIME_SYNC:
      return "TIME_SYNC";
    case BinaryPacket::PKT_ACK_SUMMARY:
      return "ACK_SUMMARY";
    default:
      return "UNKNOWN";
  }
}

}

SmartFiresBaseApp::SmartFiresBaseApp(const Config &cfg,
                                     IClock &clock,
                                     ITdmaRadioDriver &radio,
                                     HardwareSerial &jetsonUart,
                                     Print &debugUart)
    : _cfg(cfg),
      _clock(clock),
      _radio(radio),
      _jetsonUart(jetsonUart),
      _debugUart(debugUart) {}

bool SmartFiresBaseApp::begin() {
  _jetsonUart.begin(_cfg.uartBaud);

  if (!_radio.begin()) {
    _debugUart.println("[BaseApp] Radio begin failed");
    return false;
  }

  _initialized = true;
  _lastHealthLogMs = _clock.millis();
  _lastPeriodicTimeSyncMs = _lastHealthLogMs;
  _sessionId = 0x53460000UL |
               ((static_cast<uint32_t>(_cfg.baseAddr) & 0xFFu) << 8) |
               (static_cast<uint32_t>(_clock.millis()) & 0xFFu);

  _debugUart.println("[BaseApp] Ready");
  return true;
}

void SmartFiresBaseApp::update() {
  if (!_initialized) {
    return;
  }

  processIncomingLoRa();
  processIncomingJetsonUart();
  maybeSendPeriodicTimeSync();
  maybeLogHealth();
}

BinaryPacket::TimeSyncPayload SmartFiresBaseApp::baseLocalTimeSyncPayload() const {
  BinaryPacket::TimeSyncPayload ts = {};
  ts.session_id = _sessionId;
  ts.session_time_ms = _clock.millis();
  return ts;
}

void SmartFiresBaseApp::maybeSendPeriodicTimeSync() {
  const uint32_t now = _clock.millis();
  if (now - _lastPeriodicTimeSyncMs < kPeriodicTimeSyncMs) {
    return;
  }

  const BinaryPacket::TimeSyncPayload ts = currentTimeSyncPayload();
  uint8_t payload[BinaryPacket::kTimeSyncLoRaSize] = {};
  const uint8_t seq = _timeSyncSeq++;
  const uint8_t len =
      BinaryPacket::encodeTimeSyncPayload(seq, ts, payload, sizeof(payload));
  if (len == 0) {
    _debugUart.println("[BaseApp] TX TIME_SYNC periodic encode failed");
    _lastPeriodicTimeSyncMs = now;
    return;
  }

  const bool ok = _radio.send(payload, len, _cfg.timeSyncBroadcastAddr);
  if (ok) {
    _timeSyncTxCount++;
  }

  _debugUart.print("[BaseApp] TX TIME_SYNC_PERIODIC seq=");
  _debugUart.print(seq);
  _debugUart.print(" source=");
  _debugUart.print(_hasJetsonTime ? "jetson" : "base_local");
  _debugUart.print(" sessionMs=");
  _debugUart.print(ts.session_time_ms);
  _debugUart.print(" to=");
  _debugUart.print(_cfg.timeSyncBroadcastAddr);
  _debugUart.print(" result=");
  _debugUart.println(ok ? "OK" : "FAIL");

  _lastPeriodicTimeSyncMs = now;
}

void SmartFiresBaseApp::processIncomingLoRa() {
  while (_radio.available()) {
    ITdmaRadioDriver::ReceivedPacket pkt;
    if (!_radio.receive(pkt)) {
      _radioReceiveFailCount++;
      _debugUart.print("[BaseApp] RX_FAIL count=");
      _debugUart.println(_radioReceiveFailCount);
      return;
    }

    _lastRxMs = _clock.millis();

    BinaryPacket::PktHeader hdr = {};
    const bool hasHeader = pkt.len >= sizeof(BinaryPacket::PktHeader);
    const bool validHeader = hasHeader &&
                             (memcpy(&hdr, pkt.data, sizeof(BinaryPacket::PktHeader)),
                              hdr.magic == BinaryPacket::PKT_MAGIC);

    if (!validHeader) {
      _rawRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      _awakenRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_BUNDLE) {
      _bundleRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_STATUS) {
      _statusRxCount++;
    } else if (hdr.pkt_type == BinaryPacket::PKT_FULL_STATE) {
      _fullStateRxCount++;
    }

    _debugUart.print("[BaseApp] RX from=");
    _debugUart.print(pkt.from);
    _debugUart.print(" type=");
    _debugUart.print(validHeader ? pktTypeName(hdr.pkt_type) : "RAW");
    _debugUart.print(" seq=");
    _debugUart.print(validHeader ? hdr.seq : 0);
    _debugUart.print(" node=");
    _debugUart.print(validHeader ? hdr.node_id : pkt.from);
    _debugUart.print(" len=");
    _debugUart.print(pkt.len);
    _debugUart.print(" rssi=");
    _debugUart.println(pkt.rssi);

    if (validHeader && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      _debugUart.print("[BaseApp][AWAKEN#");
      _debugUart.print(_awakenRxCount);
      _debugUart.print("] node=");
      _debugUart.print(hdr.node_id);
      _debugUart.print(" seq=");
      _debugUart.print(hdr.seq);
      _debugUart.println(" action=send_local_time_sync_and_forward_to_edge");

      const bool syncOk = sendDirectTimeSync(hdr.node_id, "awaken", hdr.seq);
      _debugUart.print("[BaseApp][AWAKEN#");
      _debugUart.print(_awakenRxCount);
      _debugUart.print("] local_time_sync_result=");
      _debugUart.println(syncOk ? "OK" : "FAIL");
    }

    uint8_t frame[2 + 1 + 1 + 255 + 1] = {};
    const size_t outLen =
        BinaryPacket::encodeBaseFrame(pkt.rssi, pkt.data, pkt.len, frame, sizeof(frame));
    if (outLen == 0) {
      continue;
    }

    _jetsonUart.write(frame, outLen);
    _rxForwardCount++;

    if (validHeader && hdr.pkt_type == BinaryPacket::PKT_AWAKEN) {
      _debugUart.print("[BaseApp][AWAKEN#");
      _debugUart.print(_awakenRxCount);
      _debugUart.print("] forwarded bytes=");
      _debugUart.println(outLen);
    }
  }
}

bool SmartFiresBaseApp::sendDirectTimeSync(uint8_t nodeId,
                                           const char *reason,
                                           uint8_t triggerSeq) {
  const BinaryPacket::TimeSyncPayload ts = currentTimeSyncPayload();

  uint8_t payload[BinaryPacket::kTimeSyncLoRaSize] = {};
  const uint8_t seq = _timeSyncSeq++;
  const uint8_t len =
      BinaryPacket::encodeTimeSyncPayload(seq, ts, payload, sizeof(payload));
  if (len == 0) {
    _debugUart.println("[BaseApp] TX TIME_SYNC local encode failed");
    return false;
  }

  const bool ok = _radio.sendToWait(payload, len, nodeId);
  _timeSyncTxCount += ok ? 1u : 0u;
  _debugUart.print("[BaseApp] TX TIME_SYNC_LOCAL seq=");
  _debugUart.print(seq);
  _debugUart.print(" node=");
  _debugUart.print(nodeId);
  _debugUart.print(" sessionId=0x");
  _debugUart.print(ts.session_id, HEX);
  _debugUart.print(" sessionMs=");
  _debugUart.print(ts.session_time_ms);
  _debugUart.print(" trigger=");
  _debugUart.print(reason ? reason : "unknown");
  _debugUart.print(" trigger_seq=");
  _debugUart.print(triggerSeq);
  _debugUart.print(" source=");
  _debugUart.print(_hasJetsonTime ? "jetson" : "base_local");
  _debugUart.print(" link_ack=");
  _debugUart.print(ok ? "OK" : "NO");
  _debugUart.print(" result=");
  _debugUart.println(ok ? "OK" : "FAIL");
  return ok;
}

void SmartFiresBaseApp::updateJetsonTimeSource(
    const BinaryPacket::TimeSyncPayload &ts) {
  _hasJetsonTime = true;
  _jetsonSessionId = ts.session_id;
  _jetsonSessionMsAtUpdate = ts.session_time_ms;
  _localMsAtJetsonUpdate = _clock.millis();
}

BinaryPacket::TimeSyncPayload SmartFiresBaseApp::currentTimeSyncPayload() const {
  BinaryPacket::TimeSyncPayload ts = {};

  if (_hasJetsonTime) {
    const uint32_t elapsedMs = _clock.millis() - _localMsAtJetsonUpdate;
    ts.session_id = _jetsonSessionId;
    ts.session_time_ms = _jetsonSessionMsAtUpdate + elapsedMs;
    return ts;
  }

  ts.session_id = _sessionId;
  ts.session_time_ms = _clock.millis();
  return ts;
}

void SmartFiresBaseApp::processIncomingJetsonUart() {
  uint8_t payload[255] = {};
  uint8_t len = 0;

  while (_jetsonUart.available() > 0) {
    const int raw = _jetsonUart.read();
    if (raw < 0) {
      break;
    }

    if (pushJetsonUartByte(static_cast<uint8_t>(raw), payload, len)) {
      BinaryPacket::PktHeader hdr = {};
      const bool validHeader =
          len >= sizeof(BinaryPacket::PktHeader) &&
          (memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader)),
           hdr.magic == BinaryPacket::PKT_MAGIC);
      if (validHeader) {
        _debugUart.print("[BaseApp] UART_CMD type=");
        _debugUart.print(pktTypeName(hdr.pkt_type));
        _debugUart.print(" seq=");
        _debugUart.print(hdr.seq);
        _debugUart.print(" len=");
        _debugUart.println(len);
      }

      if (handleJetsonCommandPayload(payload, len)) {
        _cmdForwardCount++;
      }
      len = 0;
    }
  }
}

bool SmartFiresBaseApp::handleJetsonCommandPayload(const uint8_t *payload, uint8_t len) {
  if (!payload || len < sizeof(BinaryPacket::PktHeader)) {
    return false;
  }

  BinaryPacket::PktHeader hdr;
  memcpy(&hdr, payload, sizeof(BinaryPacket::PktHeader));
  if (hdr.magic != BinaryPacket::PKT_MAGIC) {
    return false;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_TIME_SYNC) {
    BinaryPacket::TimeSyncPayload ts = {};
    BinaryPacket::PktHeader ignored = {};
    if (!BinaryPacket::decodeTimeSync(payload, len, ignored, ts)) {
      return false;
    }

    updateJetsonTimeSource(ts);

    _debugUart.print("[BaseApp] RX TIME_SYNC_UART seq=");
    _debugUart.print(hdr.seq);
    _debugUart.print(" source=jetson");
    _debugUart.print(" sessionMs=");
    _debugUart.print(ts.session_time_ms);
    _debugUart.println(" action=cache_only_not_forwarded");
    return true;
  }

  if (hdr.pkt_type == BinaryPacket::PKT_ACK_SUMMARY) {
    BinaryPacket::PktHeader ignored;
    BinaryPacket::AckSummaryPayload ack;
    if (!BinaryPacket::decodeAckSummary(payload, len, ignored, ack)) {
      return false;
    }
    const bool ok = _radio.send(payload, len, ack.node_id);
    _ackTxCount += ok ? 1u : 0u;
    _debugUart.print("[BaseApp] TX ACK_SUMMARY seq=");
    _debugUart.print(hdr.seq);
    _debugUart.print(" node=");
    _debugUart.print(ack.node_id);
    _debugUart.print(" base_seq=");
    _debugUart.print(ack.ack_base_seq);
    _debugUart.print(" mask=0x");
    _debugUart.print(ack.ack_mask, HEX);
    _debugUart.print(" result=");
    _debugUart.println(ok ? "OK" : "FAIL");
    return ok;
  }

  return false;
}

bool SmartFiresBaseApp::pushJetsonUartByte(uint8_t b,
                                           uint8_t *payloadOut,
                                           uint8_t &lenOut) {
  lenOut = 0;

  switch (_uartRx.stage) {
    case UartRxState::Stage::WaitM0:
      if (b == BinaryPacket::FRAME_M0) {
        _uartRx.stage = UartRxState::Stage::WaitM1;
      }
      return false;

    case UartRxState::Stage::WaitM1:
      _uartRx.stage = (b == BinaryPacket::FRAME_M1)
                          ? UartRxState::Stage::WaitLen
                          : UartRxState::Stage::WaitM0;
      return false;

    case UartRxState::Stage::WaitLen:
      if (b == 0) {
        _uartFrameErrorCount++;
        resetJetsonUartRx();
        return false;
      }
      _uartRx.len = b;
      _uartRx.dataPos = 0;
      _uartRx.stage = UartRxState::Stage::ReadData;
      return false;

    case UartRxState::Stage::ReadData:
      _uartRx.data[_uartRx.dataPos++] = b;
      if (_uartRx.dataPos >= _uartRx.len) {
        _uartRx.stage = UartRxState::Stage::WaitCrc;
      }
      return false;

    case UartRxState::Stage::WaitCrc: {
      _uartRx.crc = b;

      uint8_t crcInput[1 + sizeof(_uartRx.data)] = {};
      crcInput[0] = _uartRx.len;
      memcpy(crcInput + 1, _uartRx.data, _uartRx.len);
      const uint8_t expected = BinaryPacket::crc8(crcInput, static_cast<size_t>(_uartRx.len + 1));

      if (_uartRx.crc != expected) {
        _uartFrameErrorCount++;
        resetJetsonUartRx();
        return false;
      }

      if (payloadOut) {
        memcpy(payloadOut, _uartRx.data, _uartRx.len);
      }
      lenOut = _uartRx.len;
      resetJetsonUartRx();
      return true;
    }
  }

  resetJetsonUartRx();
  return false;
}

void SmartFiresBaseApp::resetJetsonUartRx() {
  _uartRx.stage = UartRxState::Stage::WaitM0;
  _uartRx.len = 0;
  _uartRx.dataPos = 0;
  _uartRx.crc = 0;
}

void SmartFiresBaseApp::maybeLogHealth() {
  const uint32_t now = _clock.millis();
  if (now - _lastHealthLogMs < kHealthLogPeriodMs) {
    return;
  }

  const uint32_t lastRxAgoMs = (_lastRxMs == 0) ? 0xFFFFFFFFu : (now - _lastRxMs);

  _debugUart.print("[BaseApp] rx_fwd=");
  _debugUart.print(_rxForwardCount);
  _debugUart.print(" cmd_fwd=");
  _debugUart.print(_cmdForwardCount);
  _debugUart.print(" awaken_rx=");
  _debugUart.print(_awakenRxCount);
  _debugUart.print(" bundle_rx=");
  _debugUart.print(_bundleRxCount);
  _debugUart.print(" status_rx=");
  _debugUart.print(_statusRxCount);
  _debugUart.print(" full_rx=");
  _debugUart.print(_fullStateRxCount);
  _debugUart.print(" raw_rx=");
  _debugUart.print(_rawRxCount);
  _debugUart.print(" sync_tx=");
  _debugUart.print(_timeSyncTxCount);
  _debugUart.print(" ack_tx=");
  _debugUart.print(_ackTxCount);
  _debugUart.print(" time_src=");
  _debugUart.print(_hasJetsonTime ? "jetson" : "base_local");
  _debugUart.print(" jetson_sync_age_ms=");
  if (_hasJetsonTime) {
    _debugUart.print(now - _localMsAtJetsonUpdate);
  } else {
    _debugUart.print("n/a");
  }
  _debugUart.print(" rx_fail=");
  _debugUart.print(_radioReceiveFailCount);
  _debugUart.print(" last_rx_ms_ago=");
  if (lastRxAgoMs == 0xFFFFFFFFu) {
    _debugUart.print("never");
  } else {
    _debugUart.print(lastRxAgoMs);
  }
  _debugUart.print(" uart_err=");
  _debugUart.println(_uartFrameErrorCount);

  _lastHealthLogMs = now;
}
