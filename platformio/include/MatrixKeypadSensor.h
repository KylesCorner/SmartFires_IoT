#pragma once
#include <Arduino.h>
#include "ISensor.h"

class MatrixKeypadSensor : public ISensor {
public:
  static constexpr uint8_t kRows = 4;
  static constexpr uint8_t kCols = 4;

  MatrixKeypadSensor(const uint8_t rowPins[kRows],
                     const uint8_t colPins[kCols],
                     const char keymap[kRows][kCols],
                     const char* sensorName = "Matrix Keypad",
                     uint16_t debounceMs = 30)
      : _sensorName(sensorName),
        _debounceMs(debounceMs) {
    for (uint8_t i = 0; i < kRows; ++i) _rowPins[i] = rowPins[i];
    for (uint8_t i = 0; i < kCols; ++i) _colPins[i] = colPins[i];

    for (uint8_t r = 0; r < kRows; ++r) {
      for (uint8_t c = 0; c < kCols; ++c) {
        _keymap[r][c] = keymap[r][c];
      }
    }
  }

  const char* name() const override { return _sensorName; }

  bool begin() override {
    // Rows idle HIGH, driven LOW one at a time while scanning
    for (uint8_t r = 0; r < kRows; ++r) {
      pinMode(_rowPins[r], OUTPUT);
      digitalWrite(_rowPins[r], HIGH);
    }

    // Columns read with pullups
    for (uint8_t c = 0; c < kCols; ++c) {
      pinMode(_colPins[c], INPUT_PULLUP);
    }

    _healthy = true;
    _started = true;
    _rawKey = '\0';
    _stableKey = '\0';
    _lastRawChangeMs = millis();
    _lastSampleMs = millis();
    _lastEventMs = 0;
    _pendingKey = '\0';
    _hasPending = false;
    return true;
  }

  bool ready() const override {
    return _started;
  }

  bool sample() override {
    if (!_started) return false;

    _lastSampleMs = millis();
    const uint32_t now = _lastSampleMs;

    const char raw = scanRawKey();

    if (raw != _rawKey) {
      _rawKey = raw;
      _lastRawChangeMs = now;
    }

    // Wait for debounce period before accepting as stable
    if ((now - _lastRawChangeMs) < _debounceMs) {
      return false;
    }

    if (_stableKey != _rawKey) {
      const char previousStable = _stableKey;
      _stableKey = _rawKey;

      // Only generate an event on key press, not release
      if (previousStable == '\0' && _stableKey != '\0') {
        _pendingKey = _stableKey;
        _hasPending = true;
        _lastEventMs = now;
        return true;
      }
    }

    return false;
  }

  bool hasReading() const override {
    return _hasPending;
  }

  uint32_t ageMs() const override {
    if (!_hasPending && _lastEventMs == 0) return UINT32_MAX;
    return millis() - _lastEventMs;
  }

  bool healthy() const override {
    return _healthy;
  }

  // ---- Keypad-specific API ----

  // Returns the most recent stable key currently being held down, or '\0'
  char currentKey() const {
    return _stableKey;
  }

  // Returns the pending keypress event without consuming it
  char peekKey() const {
    return _hasPending ? _pendingKey : '\0';
  }

  // Returns the pending keypress event and clears it
  char consumeKey() {
    if (!_hasPending) return '\0';
    const char k = _pendingKey;
    _pendingKey = '\0';
    _hasPending = false;
    return k;
  }

  bool keyAvailable() const {
    return _hasPending;
  }

private:
  char scanRawKey() {
    // Drive all rows HIGH first
    for (uint8_t r = 0; r < kRows; ++r) {
      digitalWrite(_rowPins[r], HIGH);
    }

    for (uint8_t r = 0; r < kRows; ++r) {
      digitalWrite(_rowPins[r], LOW);
      delayMicroseconds(3);

      for (uint8_t c = 0; c < kCols; ++c) {
        // With pullups, pressed key pulls the column LOW
        if (digitalRead(_colPins[c]) == LOW) {
          digitalWrite(_rowPins[r], HIGH);
          return _keymap[r][c];
        }
      }

      digitalWrite(_rowPins[r], HIGH);
    }

    return '\0';
  }

private:
  const char* _sensorName;
  uint16_t _debounceMs;

  uint8_t _rowPins[kRows]{};
  uint8_t _colPins[kCols]{};
  char _keymap[kRows][kCols]{};

  bool _started = false;
  bool _healthy = false;

  char _rawKey = '\0';
  char _stableKey = '\0';

  uint32_t _lastRawChangeMs = 0;
  uint32_t _lastSampleMs = 0;
  uint32_t _lastEventMs = 0;

  char _pendingKey = '\0';
  bool _hasPending = false;
};
