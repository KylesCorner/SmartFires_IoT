#include "platform/AdafruitGpsDriver.h"

#include <Adafruit_PMTK.h>

AdafruitGpsDriver::AdafruitGpsDriver(TwoWire &wire)
    : _gps(&wire) {}

bool AdafruitGpsDriver::begin(uint8_t address) {
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

  _gps.read();

  if (_gps.newNMEAreceived()) {
    return _gps.parse(_gps.lastNMEA());
  }

  return true;
}

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
