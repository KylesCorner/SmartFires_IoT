#include "platform/AdafruitGpsDriver.h"

#include <Adafruit_PMTK.h>
#include <Arduino.h>

AdafruitGpsDriver::AdafruitGpsDriver(TwoWire &wire, uint8_t wakePin)
    : _gps(&wire), _wakePin(wakePin) {}

bool AdafruitGpsDriver::begin(uint8_t address) {
  pinMode(_wakePin, OUTPUT);
  digitalWrite(_wakePin, LOW);

  if (!_gps.begin(address)) {
    _begun = false;
    return false;
  }

  _gps.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  _gps.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);
  _gps.sendCommand(PGCMD_ANTENNA);

  _begun = true;
  return true;
}
bool AdafruitGpsDriver::poll() {
  if (!_begun) {
    return false;
  }

  constexpr uint8_t MAX_CHARS_PER_POLL = 16;

  for (uint8_t i = 0; i < MAX_CHARS_PER_POLL; ++i) {
    char c = _gps.read();

    if (c == 0) {
      break;
    }

    if (_gps.newNMEAreceived()) {
    char *sentence = _gps.lastNMEA();

      // Serial.print("[GPS RAW] ");
      // Serial.print(sentence);

      _gps.parse(sentence);
    }
  }

  return true;
}

// bool AdafruitGpsDriver::poll() {
//   if (!_begun) {
//     return false;
//   }
//
//   _gps.read();
//
//   if (_gps.newNMEAreceived()) {
//     return _gps.parse(_gps.lastNMEA());
//   }
//
//   return true;
// }

bool AdafruitGpsDriver::read(Data &out) {
  if (!_begun) {
    return false;
  }

  out.fix = _gps.fix;
  out.fixQuality = _gps.fixquality;
  out.satellites = _gps.satellites;

  out.latitudeDeg = _gps.fix ? _gps.latitudeDegrees : 0.0f;
  out.longitudeDeg = _gps.fix ? _gps.longitudeDegrees : 0.0f;
  out.altitudeM = _gps.fix ? _gps.altitude : 0.0f;

  out.hour = _gps.hour;
  out.minute = _gps.minute;
  out.second = _gps.seconds;

  return true;
}
bool AdafruitGpsDriver::enterStandby() {
  if (!_begun)
    return false;

  _gps.sendCommand("$PMTK161,0*28");
  return true;
}

bool AdafruitGpsDriver::enterBackup() {
  if (!_begun)
    return false;

  _gps.sendCommand("$PMTK225,4*2F");
  return true;
}

bool AdafruitGpsDriver::enterFullPower() {
  if (!_begun)
    return false;

  _gps.sendCommand("$PMTK225,0*2B");
  return true;
}

bool AdafruitGpsDriver::wakeFromBackup() {
  if (!_begun)
    return false;

  digitalWrite(_wakePin, HIGH);
  delay(100);
  digitalWrite(_wakePin, LOW);

  _gps.sendCommand("$PMTK225,0*2B");

  return true;
}

bool AdafruitGpsDriver::enterPeriodicStandby(const GpsPeriodicConfig &cfg) {
  return enterPeriodic(2, cfg);
}

bool AdafruitGpsDriver::enterPeriodicBackup(const GpsPeriodicConfig &cfg) {
  return enterPeriodic(1, cfg);
}

bool AdafruitGpsDriver::enterPeriodic(uint8_t type,
                                      const GpsPeriodicConfig &cfg) {
  if (!_begun)
    return false;

  char payload[96];
  snprintf(payload, sizeof(payload), "PMTK225,%u,%lu,%lu,%lu,%lu",
           static_cast<unsigned>(type),
           static_cast<unsigned long>(cfg.runTimeMs),
           static_cast<unsigned long>(cfg.sleepTimeMs),
           static_cast<unsigned long>(cfg.secondRunTimeMs),
           static_cast<unsigned long>(cfg.secondSleepTimeMs));

  return sendPmtkPayload(payload);
}

bool AdafruitGpsDriver::sendPmtkPayload(const char *payload) {
  if (!_begun || payload == nullptr)
    return false;

  uint8_t checksum = 0;
  for (const char *p = payload; *p != '\0'; ++p) {
    checksum ^= static_cast<uint8_t>(*p);
  }

  char command[128];
  snprintf(command, sizeof(command), "$%s*%02X", payload, checksum);

  // Serial.print("[GPS] PMTK command: ");
  // Serial.println(command);

  _gps.sendCommand(command);
  return true;
}

bool AdafruitGpsDriver::enterAlwaysLocateStandby() {
  return sendPmtkPayload("PMTK225,8");
}

bool AdafruitGpsDriver::enterAlwaysLocateBackup() {
  return sendPmtkPayload("PMTK225,9");
}
