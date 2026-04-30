#include "platform/SensirionUartSps30Driver.h"

SensirionUartSps30Driver::SensirionUartSps30Driver(Stream &serial)
    : _serial(serial) {}

bool SensirionUartSps30Driver::begin() {
  _sensor.begin(_serial);

  // Safe cleanup in case the sensor was already measuring from a previous reset.
  // The command can fail if the sensor is already idle; do not treat that as fatal.
  _sensor.stopMeasurement();

  _begun = true;
  _measuring = false;
  return true;
}

bool SensirionUartSps30Driver::startMeasurement() {
  if (!_begun) {
    return false;
  }

  // Wake-up sequence is useful if the sensor was previously put into low power
  // by older test code or a future sleep implementation.
  _sensor.wakeUpSequence();

  const int16_t err =
      _sensor.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);

  _measuring = (err == NO_ERROR_CODE);
  return _measuring;
}

bool SensirionUartSps30Driver::stopMeasurement() {
  if (!_begun) {
    return false;
  }

  const int16_t err = _sensor.stopMeasurement();

  if (err == NO_ERROR_CODE) {
    _measuring = false;
    return true;
  }

  return false;
}

bool SensirionUartSps30Driver::read(Data &out) {
  out = Data{};

  if (!_begun || !_measuring) {
    return false;
  }

  float nc0p5 = 0.0f;
  float nc1p0 = 0.0f;
  float nc2p5 = 0.0f;
  float nc4p0 = 0.0f;
  float nc10p0 = 0.0f;
  float typicalParticleSize = 0.0f;

  const int16_t err = _sensor.readMeasurementValuesFloat(
      out.pm1_0,
      out.pm2_5,
      out.pm4_0,
      out.pm10_0,
      nc0p5,
      nc1p0,
      nc2p5,
      nc4p0,
      nc10p0,
      typicalParticleSize);

  if (err != NO_ERROR_CODE) {
    out.valid = false;
    return false;
  }

  out.valid = true;
  return true;
}
