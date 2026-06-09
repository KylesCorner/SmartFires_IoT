#include "platform/SparkfunBmv080Driver.h"
#include "logging/DebugLogger.h"

SparkfunBmv080Driver::SparkfunBmv080Driver(TwoWire &wire) : _wire(wire) {}

bool SparkfunBmv080Driver::begin(uint8_t address) {
  _address = address;

  if (!_sensor.begin(_address, _wire)) {
    _begun = false;
    _measuring = false;
    return false;
  }

  if (!_sensor.init()) {
    _begun = false;
    _measuring = false;
    return false;
  }

  _begun = true;
  _measuring = false;
  return true;
}

bool SparkfunBmv080Driver::startMeasurement() {
  if (!_begun) {
    return false;
  }

  _measuring = _sensor.setMode(SF_BMV080_MODE_CONTINUOUS);
  return _measuring;
}

bool SparkfunBmv080Driver::stopMeasurement() {
  if (!_begun) {
    return false;
  }

  // The SparkFun wrapper does not behave like the SPS30 UART driver where
  // stopMeasurement() maps cleanly to a vendor stop command in all examples.
  // For SmartFires, treat "stopped" as no longer sampling.
  _measuring = false;
  return true;
}

bool SparkfunBmv080Driver::reset() {
  if (!_begun) {
    return begin(_address);
  }

  if (!_sensor.reset()) {
    _begun = false;
    _measuring = false;
    return false;
  }

  if (!_sensor.init()) {
    _begun = false;
    _measuring = false;
    return false;
  }

  _measuring = false;
  return true;
}

// bool SparkfunBmv080Driver::read(Data &out) {
//   out = Data{};

//   if (!_begun || !_measuring) {
//     return false;
//   }

//   if (!_sensor.readSensor()) {
//     out.valid = false;
//     return false;
//   }

//   out.pm1_0 = _sensor.PM1();
//   out.pm2_5 = _sensor.PM25();
//   out.pm10_0 = _sensor.PM10();
//   out.obstructed = _sensor.isObstructed();
//   out.valid = true;

//   return true;
// }
bool SparkfunBmv080Driver::read(Data &out) {
  out = Data{};

//   LOG_DEBUG("bmv080drv", "read_start begun=%u measuring=%u",
//             _begun ? 1 : 0,
//             _measuring ? 1 : 0);

  if (!_begun || !_measuring) {
    LOG_WARN("bmv080drv", "read_reject reason=not_measuring begun=%u measuring=%u",
             _begun ? 1 : 0,
             _measuring ? 1 : 0);
    return false;
  }

//   LOG_DEBUG("bmv080drv", "read_sensor_start");

  const bool readOk = _sensor.readSensor();

//   LOG_DEBUG("bmv080drv", "read_sensor_done ok=%u", readOk ? 1 : 0);

  if (!readOk) {
    out.valid = false;
    return false;
  }

  out.pm1_0 = _sensor.PM1();
  out.pm2_5 = _sensor.PM25();
  out.pm10_0 = _sensor.PM10();
  out.obstructed = _sensor.isObstructed();
  out.valid = true;

//   LOG_INFO("bmv080drv",
//            "read_done pm1_x100=%ld pm25_x100=%ld pm10_x100=%ld obstructed=%u",
//            static_cast<long>(out.pm1_0 * 100.0f),
//            static_cast<long>(out.pm2_5 * 100.0f),
//            static_cast<long>(out.pm10_0 * 100.0f),
//            out.obstructed ? 1 : 0);

  return true;
}