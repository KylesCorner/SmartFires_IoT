#include "app/SmartFiresBaseApp.h"

#include <Arduino.h>

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

  _debugUart.println("[BaseApp] Ready");
  return true;
}

void SmartFiresBaseApp::update() {
  if (!_initialized) {
    return;
  }

  processIncomingLoRa();
  processIncomingJetsonUart();
  maybeLogHealth();
}

void SmartFiresBaseApp::processIncomingLoRa() {
  while (_radio.available()) {
    ITdmaRadioDriver::ReceivedPacket pkt;
    if (!_radio.receive(pkt)) {
      return;
    }

    uint8_t frame[2 + 1 + 1 + 255 + 1] = {};
    const size_t outLen =
        BinaryPacket::encodeBaseFrame(pkt.rssi, pkt.data, pkt.len, frame, sizeof(frame));
    if (outLen == 0) {
      continue;
    }

    _jetsonUart.write(frame, outLen);
    _rxForwardCount++;
  }
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
    return _radio.send(payload, len, _cfg.timeSyncBroadcastAddr);
  }

  if (hdr.pkt_type == BinaryPacket::PKT_ACK_SUMMARY) {
    BinaryPacket::PktHeader ignored;
    BinaryPacket::AckSummaryPayload ack;
    if (!BinaryPacket::decodeAckSummary(payload, len, ignored, ack)) {
      return false;
    }
    return _radio.send(payload, len, ack.node_id);
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
      if (b == 0 || b > sizeof(_uartRx.data)) {
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

  _debugUart.print("[BaseApp] rx_fwd=");
  _debugUart.print(_rxForwardCount);
  _debugUart.print(" cmd_fwd=");
  _debugUart.print(_cmdForwardCount);
  _debugUart.print(" uart_err=");
  _debugUart.println(_uartFrameErrorCount);

  _lastHealthLogMs = now;
}
