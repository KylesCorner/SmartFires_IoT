#include "radio/RadioService.h"

#include <stdio.h>
#include <string.h>

RadioService::RadioService(const Config &cfg, IRadio &radio, IClock &clock)
    : _cfg(cfg), _radio(radio), _clock(clock) {}

bool RadioService::begin() {
  _error = RadioServiceError::None;

  if (!_radio.begin()) {
    _state = RadioServiceState::Error;
    _error = RadioServiceError::BeginFailed;
    return false;
  }

  _state = RadioServiceState::Ready;
  return true;
}

void RadioService::update() {
  if (_state == RadioServiceState::Off || _state == RadioServiceState::Error) {
    return;
  }

  handleReceive();

  if (_state == RadioServiceState::WaitingForAck) {
    handleAckTimeout();
  }
}

bool RadioService::sendTelemetry(const TelemetryFrame &frame) {
  if (_state != RadioServiceState::Ready) {
    return false;
  }

  char packet[TelemetryFrame::MaxLen];

  const uint16_t seq = _nextSeq++;

  const int n = snprintf(packet,
                         sizeof(packet),
                         "T,node=%u,seq=%u,%s",
                         static_cast<unsigned>(_cfg.nodeId),
                         static_cast<unsigned>(seq),
                         frame.payload);

  if (n < 0 || static_cast<size_t>(n) >= sizeof(packet)) {
    _error = RadioServiceError::SendFailed;
    return false;
  }

  strncpy(_lastPayload, packet, sizeof(_lastPayload));
  _lastPayload[sizeof(_lastPayload) - 1] = '\0';
  _lastPayloadLen = strlen(_lastPayload);

  _waitingSeq = seq;
  _retryCount = 0;

  if (!sendRaw(_lastPayload, _lastPayloadLen)) {
    _state = RadioServiceState::Error;
    _error = RadioServiceError::SendFailed;
    return false;
  }

  if (_cfg.requireAck) {
    _state = RadioServiceState::WaitingForAck;
  } else {
    _state = RadioServiceState::Ready;
  }

  return true;
}

bool RadioService::hasCommand() const {
  return _hasCommand;
}

size_t RadioService::readCommand(char *out, size_t maxLen) {
  if (!out || maxLen == 0 || !_hasCommand) {
    return 0;
  }

  const size_t copyLen =
      (_commandLen < maxLen - 1) ? _commandLen : maxLen - 1;

  memcpy(out, _commandBuf, copyLen);
  out[copyLen] = '\0';

  _hasCommand = false;
  _commandLen = 0;
  _commandBuf[0] = '\0';

  return copyLen;
}

RadioServiceState RadioService::state() const {
  return _state;
}

RadioServiceError RadioService::error() const {
  return _error;
}

uint16_t RadioService::lastSeq() const {
  return _waitingSeq;
}

uint8_t RadioService::retryCount() const {
  return _retryCount;
}

bool RadioService::sendRaw(const char *payload, size_t len) {
  if (!payload || len == 0) {
    return false;
  }

  const bool ok = _radio.send(reinterpret_cast<const uint8_t *>(payload), len);

  if (ok) {
    _lastSendMs = _clock.millis();
  }

  return ok;
}

bool RadioService::resendLast() {
  if (_lastPayloadLen == 0) {
    return false;
  }

  return sendRaw(_lastPayload, _lastPayloadLen);
}

void RadioService::handleReceive() {
  while (_radio.available()) {
    char msg[CommandBufLen];
    const size_t n =
        _radio.receive(reinterpret_cast<uint8_t *>(msg), sizeof(msg) - 1);

    if (n >= sizeof(msg)) {
      _state = RadioServiceState::Error;
      _error = RadioServiceError::ReceiveOverflow;
      return;
    }

    msg[n] = '\0';

    uint16_t ackSeq = 0;

    if (parseAck(msg, ackSeq)) {
      if (_state == RadioServiceState::WaitingForAck &&
          ackSeq == _waitingSeq) {
        _state = RadioServiceState::Ready;
        _error = RadioServiceError::None;
      }

      continue;
    }

    parseCommand(msg);
  }
}

void RadioService::handleAckTimeout() {
  if (_clock.millis() - _lastSendMs < _cfg.ackTimeoutMs) {
    return;
  }

  if (_retryCount >= _cfg.maxRetries) {
    _state = RadioServiceState::Error;
    _error = RadioServiceError::AckTimeout;
    return;
  }

  _retryCount++;

  if (!resendLast()) {
    _state = RadioServiceState::Error;
    _error = RadioServiceError::SendFailed;
  }
}

bool RadioService::parseAck(const char *msg, uint16_t &seqOut) const {
  if (!msg) {
    return false;
  }

  unsigned seq = 0;

  if (sscanf(msg, "ACK,%u", &seq) == 1) {
    seqOut = static_cast<uint16_t>(seq);
    return true;
  }

  return false;
}

bool RadioService::parseCommand(const char *msg) {
  if (!msg) {
    return false;
  }

  constexpr const char *prefix = "CMD,";
  constexpr size_t prefixLen = 4;

  if (strncmp(msg, prefix, prefixLen) != 0) {
    return false;
  }

  const char *cmd = msg + prefixLen;
  const size_t len = strlen(cmd);

  if (len >= CommandBufLen) {
    _state = RadioServiceState::Error;
    _error = RadioServiceError::ReceiveOverflow;
    return false;
  }

  memcpy(_commandBuf, cmd, len);
  _commandBuf[len] = '\0';
  _commandLen = len;
  _hasCommand = true;

  return true;
}
