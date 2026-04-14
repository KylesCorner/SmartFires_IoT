#pragma once
class UartLoRaBridge {
public:
  static constexpr uint32_t kTelemetryPeriodMs = 250;
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

      // Serial.print("[UART RAW RX] ");
      // Serial.print(c);
      // Serial.print(" '");
      // if (c >= 32 && c <= 126) Serial.print((char)c);
      // else Serial.print('.');
      // Serial.println("'");

      if (c == '\r') continue;

      if (c == '\n') {
        _line[_lineLen] = '\0';
        if (_lineLen > 0) {
          // Serial.print("[UART LINE RX] ");
          // Serial.println(_line);
          handleLine(_line);
        }
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

  // Send a pre-built binary frame (no framing added — caller owns the full frame bytes).
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

private:
  void handleLine(const char* line) {
    // Serial.print("[Feather->ESP32] ");
    // Serial.println(line);

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

  HardwareSerial& _port;
  int _rxPin;
  int _txPin;
  uint32_t _baud;

  char _line[160] = {0};
  size_t _lineLen = 0;

  bool _bootSeen = false;
  bool _hasAck = false;
  bool _hasRx = false;
  bool _hasError = false;

  uint32_t _lastAckSeq = 0;
  char _lastRx[96] = {0};
  char _lastError[64] = {0};
  bool _hasLoraAck = false;
};
