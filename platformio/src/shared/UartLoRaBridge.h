#pragma once
#include "shared/BinaryPacket.h"

class UartLoRaBridge {
public:
  static constexpr uint32_t kTelemetryPeriodMs = 250;   // 4 Hz sensing rate
  static constexpr uint32_t kAckTimeoutMs = 300;

  UartLoRaBridge(HardwareSerial& port, int rxPin, int txPin, uint32_t baud = 115200)
    : _port(port), _rxPin(rxPin), _txPin(txPin), _baud(baud) {}

  void begin() {
    _port.begin(_baud, SERIAL_8N1, _rxPin, _txPin);
  }

  void update() {
    while (_port.available() > 0) {
      const int c = _port.read();
      if (c < 0) break;

      if (_binMode) {
        handleBinaryByte(static_cast<uint8_t>(c));
        continue;
      }

      if (c == '\r') continue;

      // Binary frames from the Feather start with 0xAA at the beginning of a message.
      if (_lineLen == 0 && c == BinaryPacket::FRAME_M0) {
        _binMode  = true;
        _binState = BIN_WAIT_M1;
        continue;
      }

      if (c == '\n') {
        _line[_lineLen] = '\0';
        if (_lineLen > 0) handleLine(_line);
        _lineLen = 0;
        continue;
      }

      if (_lineLen + 1 < sizeof(_line)) {
        _line[_lineLen++] = static_cast<char>(c);
      } else {
        _lineLen = 0;
        strncpy(_lastError, "uart_overflow", sizeof(_lastError) - 1);
        _lastError[sizeof(_lastError) - 1] = '\0';
      }
    }
  }

  void sendLine(const char* s) {
    _port.print(s);
    _port.print('\n');
  }

  void sendBinaryFrame(const uint8_t* data, size_t len) {
    _port.write(data, len);
  }

  bool hasAck() const { return _hasAck; }
  uint32_t lastAckSeq() const { return _lastAckSeq; }

  bool hasRx() const { return _hasRx; }
  const char* lastRx() const { return _lastRx; }

  bool hasError() const { return _hasError; }
  const char* lastError() const { return _lastError; }

  bool bootSeen() const { return _bootSeen; }
  bool hasLoraAck() const { return _hasLoraAck; }

  bool hasTimeSync() const { return _hasTimeSync; }
  uint32_t lastSessionId() const { return _lastSessionId; }
  uint32_t lastSessionTimeMs() const { return _lastSessionTimeMs; }
  void clearTimeSync() { _hasTimeSync = false; }

private:
  // ---- text line parser ----

  void handleLine(const char* line) {
    if (strncmp(line, "ACK,BOOT", 8) == 0) {
      _bootSeen = true;
      return;
    }
    if (strncmp(line, "ACK,", 4) == 0) {
      _hasAck = true;
      _lastAckSeq = static_cast<uint32_t>(strtoul(line + 4, nullptr, 10));
      return;
    }
    if (strncmp(line, "RX,", 3) == 0) {
      _hasRx = true;
      strncpy(_lastRx, line + 3, sizeof(_lastRx) - 1);
      _lastRx[sizeof(_lastRx) - 1] = '\0';
      return;
    }
    if (strncmp(line, "ERR,", 4) == 0) {
      _hasError = true;
      strncpy(_lastError, line + 4, sizeof(_lastError) - 1);
      _lastError[sizeof(_lastError) - 1] = '\0';
      return;
    }
  }

  // ---- binary frame parser (for TIME_SYNC frames from Feather) ----

  enum BinState : uint8_t { BIN_WAIT_M1, BIN_WAIT_LEN, BIN_READ_DATA, BIN_CHECK_CRC };

  void handleBinaryByte(uint8_t b) {
    switch (_binState) {
      case BIN_WAIT_M1:
        if (b == BinaryPacket::FRAME_M1) {
          _binState = BIN_WAIT_LEN;
        } else {
          _binMode = false;  // not a valid frame, back to text
        }
        break;

      case BIN_WAIT_LEN:
        _binExpected = b;
        _binPos = 0;
        if (_binExpected == 0 || static_cast<size_t>(1 + _binExpected) > sizeof(_binBuf)) {
          _binMode = false;
        } else {
          _binBuf[_binPos++] = b;  // len byte at [0], included in CRC
          _binState = BIN_READ_DATA;
        }
        break;

      case BIN_READ_DATA:
        if (_binPos < sizeof(_binBuf)) _binBuf[_binPos++] = b;
        if (_binPos == static_cast<uint8_t>(1 + _binExpected)) _binState = BIN_CHECK_CRC;
        break;

      case BIN_CHECK_CRC: {
        const uint8_t computed = BinaryPacket::crc8(_binBuf, 1 + _binExpected);
        if (b == computed) {
          processBinaryFrame(_binBuf + 1, _binExpected);
        } else {
          Serial.print("[BRIDGE] binary CRC mismatch: got 0x");
          Serial.print(b, HEX);
          Serial.print(" expected 0x");
          Serial.println(computed, HEX);
        }
        _binMode = false;
        break;
      }
    }
  }

  void processBinaryFrame(const uint8_t* data, uint8_t len) {
    BinaryPacket::PktHeader    hdr{};
    BinaryPacket::TimeSyncPayload ts{};
    if (BinaryPacket::decodeTimeSync(data, len, hdr, ts)) {
      _hasTimeSync       = true;
      _lastSessionId     = ts.session_id;
      _lastSessionTimeMs = ts.session_time_ms;
      Serial.print("[BRIDGE] TIME_SYNC decoded session_id=");
      Serial.print(ts.session_id);
      Serial.print(" session_ms=");
      Serial.println(ts.session_time_ms);
    } else {
      Serial.println("[BRIDGE] binary frame decode failed (bad magic/type)");
    }
  }

  // ---- data members ----

  HardwareSerial& _port;
  int      _rxPin;
  int      _txPin;
  uint32_t _baud;

  // text parse state
  char   _line[160] = {0};
  size_t _lineLen   = 0;

  // binary frame parse state
  bool     _binMode    = false;
  BinState _binState   = BIN_WAIT_M1;
  uint8_t  _binBuf[20] = {0};  // len(1) + data(up to 12 for TIME_SYNC) + slack
  uint8_t  _binExpected = 0;
  uint8_t  _binPos      = 0;

  // text ACK/Rx/error flags
  bool     _bootSeen   = false;
  bool     _hasAck     = false;
  bool     _hasRx      = false;
  bool     _hasError   = false;
  bool     _hasLoraAck = false;
  uint32_t _lastAckSeq = 0;
  char     _lastRx[96]    = {0};
  char     _lastError[64] = {0};

  // TIME_SYNC state
  bool     _hasTimeSync       = false;
  uint32_t _lastSessionId     = 0;
  uint32_t _lastSessionTimeMs = 0;
};
