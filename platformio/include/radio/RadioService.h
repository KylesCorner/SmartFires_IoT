#pragma once

#include "interfaces/IClock.h"
#include "interfaces/IRadio.h"
#include "telemetry/TelemetryFrame.h"

#include <stddef.h>
#include <stdint.h>

enum class RadioServiceState : uint8_t {
  Off,
  Ready,
  WaitingForAck,
  Error
};

enum class RadioServiceError : uint8_t {
  None,
  BeginFailed,
  SendFailed,
  AckTimeout,
  ReceiveOverflow
};

class RadioService {
public:
  struct Config {
    uint8_t nodeId = 1;
    uint8_t maxRetries = 3;
    uint32_t ackTimeoutMs = 1000;
    bool requireAck = true;
  };

  RadioService(const Config &cfg, IRadio &radio, IClock &clock);

  bool begin();
  void update();

  bool sendTelemetry(const TelemetryFrame &frame);

  bool hasCommand() const;
  size_t readCommand(char *out, size_t maxLen);

  RadioServiceState state() const;
  RadioServiceError error() const;

  uint16_t lastSeq() const;
  uint8_t retryCount() const;

private:
  static constexpr size_t CommandBufLen = 96;

  Config _cfg;
  IRadio &_radio;
  IClock &_clock;

  RadioServiceState _state = RadioServiceState::Off;
  RadioServiceError _error = RadioServiceError::None;

  uint16_t _nextSeq = 1;
  uint16_t _waitingSeq = 0;
  uint8_t _retryCount = 0;
  uint32_t _lastSendMs = 0;

  char _lastPayload[TelemetryFrame::MaxLen] = {};
  size_t _lastPayloadLen = 0;

  char _commandBuf[CommandBufLen] = {};
  size_t _commandLen = 0;
  bool _hasCommand = false;

  bool sendRaw(const char *payload, size_t len);
  bool resendLast();

  void handleReceive();
  void handleAckTimeout();

  bool parseAck(const char *msg, uint16_t &seqOut) const;
  bool parseCommand(const char *msg);
};
