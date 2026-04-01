// Pa1010dGpsSensor.cpp
#include "Pa1010dGpsSensor.h"

Pa1010dGpsSensor::Pa1010dGpsSensor(TwoWire& wire, uint8_t address)
  : _wire(&wire), _address(address), _gps(&wire) {}

bool Pa1010dGpsSensor::begin(TwoWire& wire) {
  // This wrapper binds the transport in the constructor.
  // For a normal Nano setup, just construct with Wire and call begin().
  if (&wire != _wire) {
    _healthy = false;
    _begun = false;
    return false;
  }

  _wire->begin();

  if (!ping()) {
    _healthy = false;
    _begun = false;
    return false;
  }

  if (!_gps.begin(_address)) {
    _healthy = false;
    _begun = false;
    return false;
  }

  _begun = true;
  _healthy = true;
  _hasReading = false;
  _consecutiveParseFailures = 0;
  _lastByteMs = millis();
  _lastSentenceMs = 0;
  _lastFixMs = 0;

  return configureGps_();
}

bool Pa1010dGpsSensor::ping() {
  _wire->beginTransmission(_address);
  const uint8_t err = _wire->endTransmission();

  const bool ok = (err == 0);
  if (!ok) {
    _healthy = false;
  }
  return ok;
}

bool Pa1010dGpsSensor::configureGps_() {
  // Match Adafruit's recommended parsing setup:
  // - RMC + GGA sentences
  // - 1 Hz update rate
  // - 1 Hz fix rate
  _gps.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  _gps.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);
  _gps.sendCommand(PMTK_API_SET_FIX_CTL_1HZ);

  return true;
}

bool Pa1010dGpsSensor::sample() {
  if (!_begun) {
    return false;
  }

  bool updated = false;

  // Drain a bounded number of chars so this stays non-blocking.
  uint8_t charsRead = 0;
  while (_gps.available() && charsRead < kDefaultCharsPerSample) {
    (void)_gps.read();
    _lastByteMs = millis();
    ++charsRead;
  }

  // Parse any completed NMEA sentence(s).
  while (_gps.newNMEAreceived()) {
    char* nmea = _gps.lastNMEA();

    if (_gps.parse(nmea)) {
      const uint32_t now = millis();

      _consecutiveParseFailures = 0;
      _hasReading = true;
      _healthy = true;
      _lastSentenceMs = now;

      copyReading_();

      if (_reading.fix) {
        _lastFixMs = now;
      }

      updated = true;
    } else {
      if (_consecutiveParseFailures < 255) {
        ++_consecutiveParseFailures;
      }

      if (_consecutiveParseFailures >= 10) {
        _healthy = false;
      }
    }
  }

  // If the bus goes quiet for a long time, flag unhealthy.
  if (_lastByteMs != 0 && (millis() - _lastByteMs) > 5000UL) {
    _healthy = false;
  }

  return updated;
}

uint32_t Pa1010dGpsSensor::ageMs() const {
  if (_lastFixMs == 0) {
    return UINT32_MAX;
  }
  return millis() - _lastFixMs;
}

uint32_t Pa1010dGpsSensor::sentenceAgeMs() const {
  if (_lastSentenceMs == 0) {
    return UINT32_MAX;
  }
  return millis() - _lastSentenceMs;
}

void Pa1010dGpsSensor::copyReading_() {
  _reading.fix = _gps.fix;
  _reading.fixQuality = _gps.fixquality;
  _reading.satellites = _gps.satellites;

  _reading.latitudeDeg = static_cast<float>(_gps.latitudeDegrees);
  _reading.longitudeDeg = static_cast<float>(_gps.longitudeDegrees);
  _reading.altitudeM = static_cast<float>(_gps.altitude);
  _reading.speedKnots = static_cast<float>(_gps.speed);
  _reading.courseDeg = static_cast<float>(_gps.angle);
  _reading.hdop = static_cast<float>(_gps.HDOP);

  _reading.hour = _gps.hour;
  _reading.minute = _gps.minute;
  _reading.seconds = _gps.seconds;
  _reading.milliseconds = _gps.milliseconds;

  _reading.day = _gps.day;
  _reading.month = _gps.month;
  _reading.year = _gps.year;
}