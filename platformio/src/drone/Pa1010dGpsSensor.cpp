// Pa1010dGpsSensor.cpp
#include "Pa1010dGpsSensor.h"

namespace {
constexpr char kCmdStandby[] = "$PMTK161,0*28";
constexpr char kCmdFullPower[] = "$PMTK225,0*2B";
constexpr char kCmdBackup[] = "$PMTK225,4*2F";
} // namespace

Pa1010dGpsSensor::Pa1010dGpsSensor(TwoWire &wire, uint8_t address,
                                   int8_t wakePin)
    : _wire(&wire), _address(address), _wakePin(wakePin), _gps(&wire) {}

bool Pa1010dGpsSensor::begin(TwoWire &wire) {
  if (&wire != _wire) {
    _healthy = false;
    _begun = false;
    return false;
  }

  // _wire->begin();

  if (_wakePin >= 0) {
    pinMode(_wakePin, OUTPUT);
    digitalWrite(_wakePin, HIGH); // inactive/high by default
  }

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
  _lastWakeMs = millis();
  _powerMode = PowerMode::FullPower;

  return configureGps_();
}

bool Pa1010dGpsSensor::ping() {
  if (_wire == nullptr)
    return false;

  _wire->beginTransmission(_address);
  const uint8_t err = _wire->endTransmission();

  const bool ok = (err == 0);
  if (!ok) {
    _healthy = false;
  }
  return ok;
}

bool Pa1010dGpsSensor::configureGps_() {
  // Default operating mode after wake / boot
  _gps.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  _gps.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);
  _gps.sendCommand(PMTK_API_SET_FIX_CTL_1HZ);

  return true;
}

bool Pa1010dGpsSensor::ready() const {
  if (!_begun || !_healthy)
    return false;
  if (_powerMode != PowerMode::FullPower)
    return false;
  if ((millis() - _lastWakeMs) < kWakeSettleMs)
    return false;
  return _hasReading && _reading.fix;
}

bool Pa1010dGpsSensor::sleep() {
  Serial.println("sleeping gps");
  // if (!_begun || !_healthy)
  //   return false;
  // if (_powerMode != PowerMode::FullPower)
  //   return false;

  // Standby mode: wakes via interface activity.
  _gps.sendCommand(kCmdStandby);

  _powerMode = PowerMode::Standby;
  _hasReading = false;
  clearReading_();
  return true;
}
bool Pa1010dGpsSensor::wake() {
  // if (!_begun) {
  //   return false;
  // }

  if (_powerMode == PowerMode::Backup) {
    return wakeFromBackup();
  }

  if (_powerMode == PowerMode::FullPower) {
    return true;
  }

  // For standby, do a gentle wake attempt first.
  // Avoid assuming the PMTK full-power command is safe immediately.
  if (_wire == nullptr) {
    return false;
  }

  // Wake stimulus: a plain I2C transaction is often safer than jumping
  // straight into full configuration traffic.
  _wire->beginTransmission(_address);
  uint8_t err = _wire->endTransmission(true);

  if (err != 0) {
    delay(50);

    _wire->beginTransmission(_address);
    err = _wire->endTransmission(true);

    if (err != 0) {
      _healthy = false;
      return false;
    }
  }

  // Give the GPS real time to wake up.
  delay(100);

  _powerMode = PowerMode::FullPower;
  _lastWakeMs = millis();
  _healthy = true;
  _hasReading = false;
  _consecutiveParseFailures = 0;
  clearReading_();

  // Delay before reconfiguring
  delay(100);

  return configureGps_();
}

// bool Pa1010dGpsSensor::wake() {
//   Serial.println("waking gps");
//   if (!_begun)
//     return false;
//
//   // if (_powerMode == PowerMode::FullPower) {
//   //   return false;
//   // }
//
//   if (_powerMode == PowerMode::Standby) {
//     // Datasheet: any byte wakes from standby.
//     // Sending the full-power PMTK is a reasonable wake stimulus on I2C.
//     _gps.sendCommand(kCmdFullPower);
//   } else if (_powerMode == PowerMode::Backup) {
//     return wakeFromBackup();
//   } else {
//     return true;
//   }
//
//   delay(10);
//   _powerMode = PowerMode::FullPower;
//   _lastWakeMs = millis();
//   _healthy = true;
//   _consecutiveParseFailures = 0;
//
//   return configureGps_();
// }

bool Pa1010dGpsSensor::enterBackupMode() {
  if (!_begun || !_healthy)
    return false;
  if (_wakePin < 0)
    return false; // no hardware wake control available
  if (_powerMode != PowerMode::FullPower)
    return false;

  // Datasheet: WAKE-UP pin must be tied low before entering backup.
  digitalWrite(_wakePin, LOW);
  delay(2);

  _gps.sendCommand(kCmdBackup);

  _powerMode = PowerMode::Backup;
  _hasReading = false;
  clearReading_();
  return true;
}

bool Pa1010dGpsSensor::wakeFromBackup() {
  if (!_begun)
    return false;
  if (_wakePin < 0)
    return false;
  if (_powerMode != PowerMode::Backup)
    return false;

  // Datasheet: WAKE-UP high wakes from backup.
  digitalWrite(_wakePin, HIGH);
  delay(10);

  _powerMode = PowerMode::FullPower;
  _lastWakeMs = millis();
  _healthy = true;
  _consecutiveParseFailures = 0;

  return configureGps_();
}

bool Pa1010dGpsSensor::sample() {
  if (!_begun) {
    return false;
  }

  if (_powerMode != PowerMode::FullPower) {
    return false;
  }

  bool updated = false;

  uint8_t charsRead = 0;
  while (_gps.available() && charsRead < kDefaultCharsPerSample) {
    (void)_gps.read();
    _lastByteMs = millis();
    ++charsRead;
  }

  while (_gps.newNMEAreceived()) {
    char *nmea = _gps.lastNMEA();

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

  if (_lastByteMs != 0 && (millis() - _lastByteMs) > kQuietTimeoutMs) {
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

void Pa1010dGpsSensor::clearReading_() { _reading = Reading{}; }
