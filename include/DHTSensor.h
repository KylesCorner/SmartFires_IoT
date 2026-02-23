#pragma once
#include <Arduino.h>
#include <DHT.h>
#include "ISensor.h"

class DhtSensor : public ISensor {
public:
  DhtSensor(uint8_t pin, uint8_t type, const char* sensorName = "DHT")
    : _dht(pin, type), _name(sensorName) {}

  const char* name() const override { return _name; }

  bool begin() override {
    _dht.begin();
    _hasReading = false;
    _healthy = true;
    _t0 = millis();
    return true; // library doesn't really report init failures
  }

  bool ready() const override {
    // DHT needs time between reads (typ 2s for DHT22). We'll enforce in sample().
    return true;
  }

  bool sample() override {
    const uint32_t now = millis();
    if (now - _lastSampleMs < _minIntervalMs) return false;

    _lastSampleMs = now;

    float h = _dht.readHumidity();
    float t = _dht.readTemperature();

    if (isnan(h) || isnan(t)) {
      _failCount++;
      if (_failCount >= 3) _healthy = false;
      return false;
    }

    _humidity = h;
    _tempC = t;
    _hasReading = true;
    _healthy = true;
    _failCount = 0;
    _lastReadingMs = now;
    return true;
  }

  uint32_t ageMs() const override {
    if (!_hasReading) return UINT32_MAX;
    return millis() - _lastReadingMs;
  }

  bool healthy() const override { return _healthy; }

  // Sensor-specific getters
  bool hasReading() const { return _hasReading; }
  float tempC() const { return _tempC; }
  float humidity() const { return _humidity; }

private:
  DHT _dht;
  const char* _name;

  static constexpr uint32_t _minIntervalMs = 2000;

  bool _hasReading = false;
  bool _healthy = true;
  uint8_t _failCount = 0;

  float _tempC = NAN;
  float _humidity = NAN;

  uint32_t _t0 = 0;
  uint32_t _lastSampleMs = 0;
  uint32_t _lastReadingMs = 0;
};