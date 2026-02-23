#pragma once
#include <Arduino.h>
#include "ISensor.h"

/**
 * Flame sensor (typical IR flame module).
 *
 * Supports:
 *  - Analog pin (AO): read intensity-like value (0..1023 on Uno).
 *  - Digital pin (DO): thresholded detect (HIGH/LOW depends on module).
 *
 * Notes:
 *  - Many modules are "active-low" on DO: DO == LOW means flame detected.
 *  - Use calibrate / set thresholds based on your module and environment.
 */
class FlameSensor : public ISensor {
public:
  enum class DigitalPolarity : uint8_t {
    ActiveHigh, // DO == HIGH means flame detected
    ActiveLow   // DO == LOW means flame detected (common)
  };

  // If you only have AO: pass digitalPin = 255
  // If you only have DO: pass analogPin = A0? (or 255) and set useAnalog=false
  FlameSensor(uint8_t analogPin,
              uint8_t digitalPin,
              DigitalPolarity polarity = DigitalPolarity::ActiveLow,
              const char* sensorName = "Flame")
    : _analogPin(analogPin),
      _digitalPin(digitalPin),
      _polarity(polarity),
      _name(sensorName) {}

  const char* name() const override { return _name; }

  bool begin() override {
    _tBeginMs = millis();
    _healthy = true;
    _hasReading = false;

    if (_analogPin != kNoPin) {
      pinMode(_analogPin, INPUT);
    }
    if (_digitalPin != kNoPin) {
      pinMode(_digitalPin, INPUT);
    }

    // crude warmup time: allow sensor module to stabilize
    _warmupUntilMs = _tBeginMs + _warmupMs;
    return true;
  }

  bool ready() const override {
    return (millis() >= _warmupUntilMs);
  }

  bool sample() override {
    const uint32_t now = millis();
    if (!ready()) return false;
    if (now - _lastSampleMs < _minIntervalMs) return false;

    _lastSampleMs = now;

    bool any = false;

    if (_analogPin != kNoPin) {
      const int raw = analogRead(_analogPin); // 0..1023 on Uno
      _analogRaw = raw;
      any = true;
    }

    if (_digitalPin != kNoPin) {
      const int d = digitalRead(_digitalPin);
      _digitalRaw = (d != 0);
      _detected = interpretDigital(_digitalRaw);
      any = true;
    } else {
      // If no DO pin, derive detected from analog threshold if configured
      if (_analogPin != kNoPin && _useAnalogThreshold) {
        _detected = interpretAnalog(_analogRaw);
      }
    }

    if (any) {
      _hasReading = true;
      _lastReadingMs = now;
      _healthy = true;
    }

    return any;
  }

  uint32_t ageMs() const override {
    if (!_hasReading) return UINT32_MAX;
    return millis() - _lastReadingMs;
  }

  bool healthy() const override { return _healthy; }

  // ---- Sensor-specific API ----
  bool hasReading() const { return _hasReading; }

  // Analog raw (0..1023). Valid if analog pin provided.
  int analogRaw() const { return _analogRaw; }

  // Digital raw (true/false). Valid if digital pin provided.
  bool digitalRaw() const { return _digitalRaw; }

  // Interpreted flame detection (based on DO or analog threshold)
  bool detected() const { return _detected; }

  // Optional: enable analog-threshold detection if DO pin not used
  void enableAnalogThreshold(int threshold, bool detectWhenBelow = true) {
    _useAnalogThreshold = true;
    _analogThreshold = threshold;
    _detectWhenBelow = detectWhenBelow;
  }

  void disableAnalogThreshold() { _useAnalogThreshold = false; }

private:
  static constexpr uint8_t  kNoPin = 255;
  static constexpr uint32_t _warmupMs = 200;     // module stabilization
  static constexpr uint32_t _minIntervalMs = 50; // sample every 50ms

  bool interpretDigital(bool raw) const {
    // raw == true means HIGH
    if (_polarity == DigitalPolarity::ActiveLow) return raw;
    return !raw; // active-low
  }

  bool interpretAnalog(int raw) const {
    // Many modules: more IR -> lower AO voltage (often raw decreases near flame),
    // but this varies. We let you choose direction.
    if (_detectWhenBelow) return (raw <= _analogThreshold);
    return (raw >= _analogThreshold);
  }

  uint8_t _analogPin;
  uint8_t _digitalPin;
  DigitalPolarity _polarity;
  const char* _name;

  bool _healthy = true;
  bool _hasReading = false;

  int  _analogRaw = -1;
  bool _digitalRaw = false;
  bool _detected = false;

  bool _useAnalogThreshold = false;
  int  _analogThreshold = 400;
  bool _detectWhenBelow = true;

  uint32_t _tBeginMs = 0;
  uint32_t _warmupUntilMs = 0;
  uint32_t _lastSampleMs = 0;
  uint32_t _lastReadingMs = 0;
};