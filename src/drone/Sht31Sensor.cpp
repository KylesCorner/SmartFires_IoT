#include "Sht31Sensor.h"

Sht31Sensor::Sht31Sensor(uint8_t address)
    : _address(address),
      _wire(&Wire),
      _driverConstructed(false),
      _begun(false),
      _healthy(true),
      _hasReading(false),
      _consecutiveFailures(0),
      _lastSampleMs(0),
      _temperatureC(NAN),
      _humidityPct(NAN),
      _lastI2CStatus(I2CStatus::UnknownError) {
  constructDriver(Wire);
}

Sht31Sensor::~Sht31Sensor() {
  destroyDriver();
}

const char* Sht31Sensor::name() const {
  return "Adafruit SHT31";
}

uint8_t Sht31Sensor::address() const {
  return _address;
}

bool Sht31Sensor::begin() {
  return begin(Wire);
}

bool Sht31Sensor::begin(TwoWire& wire) {
  if (!_driverConstructed || _wire != &wire) {
    constructDriver(wire);
  }

  _wire = &wire;

  if (!driver()->begin(_address)) {
    _begun = false;
    _healthy = false;

    // Try to refine the failure cause with a basic bus probe.
    ping();
    return false;
  }

  _begun = true;
  _healthy = true;
  _consecutiveFailures = 0;
  _lastI2CStatus = I2CStatus::Ok;

  // Usually best left off unless you specifically need it.
  driver()->heater(false);

  return true;
}

bool Sht31Sensor::ready() const {
  return _begun;
}

bool Sht31Sensor::sample() {
  if (!_begun) {
    return false;
  }

  float t = NAN;
  float h = NAN;

  const bool ok = driver()->readBoth(&t, &h);

  if (!ok || isnan(t) || isnan(h)) {
    ping();  // update _lastI2CStatus
    return markFailure(_lastI2CStatus);
  }

  _temperatureC = t;
  _humidityPct = h;
  _lastSampleMs = millis();
  _hasReading = true;
  _healthy = true;
  _consecutiveFailures = 0;
  _lastI2CStatus = I2CStatus::Ok;

  return true;
}

uint32_t Sht31Sensor::ageMs() const {
  if (!_hasReading) {
    return UINT32_MAX;
  }
  return millis() - _lastSampleMs;
}

bool Sht31Sensor::healthy() const {
  return _healthy;
}

bool Sht31Sensor::ping() {
  if (_wire == nullptr) {
    _lastI2CStatus = I2CStatus::UnknownError;
    return false;
  }

  _wire->beginTransmission(_address);
  const uint8_t wireStatus = _wire->endTransmission();
  _lastI2CStatus = decodeWireStatus(wireStatus);

  return wireStatus == 0;
}

bool Sht31Sensor::hasReading() const {
  return _hasReading;
}

float Sht31Sensor::temperatureC() const {
  return _temperatureC;
}


float Sht31Sensor::temperatureF() const {
  return _temperatureC * 9.0 / 5.0 + 32.0;
}

float Sht31Sensor::humidityPct() const {
  return _humidityPct;
}

void Sht31Sensor::setHeater(bool enabled) {
  if (_begun) {
    driver()->heater(enabled);
  }
}

bool Sht31Sensor::heaterEnabled() const {
  if (!_begun) {
    return false;
  }

  // Adafruit API method is non-const, so we cast here.
  return const_cast<Adafruit_SHT31*>(driver())->isHeaterEnabled();
}

void Sht31Sensor::reset() {
  if (_begun) {
    driver()->reset();
  }
}

I2CStatus Sht31Sensor::lastI2CStatus() const {
  return _lastI2CStatus;
}

void Sht31Sensor::constructDriver(TwoWire& wire) {
  destroyDriver();
  _wire = &wire;
  new (_driverStorage) Adafruit_SHT31(&wire);
  _driverConstructed = true;
}

void Sht31Sensor::destroyDriver() {
  if (_driverConstructed) {
    driver()->~Adafruit_SHT31();
    _driverConstructed = false;
  }
}

Adafruit_SHT31* Sht31Sensor::driver() {
  return reinterpret_cast<Adafruit_SHT31*>(_driverStorage);
}

const Adafruit_SHT31* Sht31Sensor::driver() const {
  return reinterpret_cast<const Adafruit_SHT31*>(_driverStorage);
}

I2CStatus Sht31Sensor::decodeWireStatus(uint8_t wireStatus) {
  // Common Arduino Wire endTransmission() codes:
  // 0 = success
  // 1 = data too long for buffer
  // 2 = NACK on address
  // 3 = NACK on data
  // 4 = other error
  // 5 = timeout (on some cores, including ESP32 variants)
  switch (wireStatus) {
    case 0: return I2CStatus::Ok;
    case 1: return I2CStatus::BufferTooSmall;
    case 2: return I2CStatus::NackOnAddress;
    case 3: return I2CStatus::NackOnData;
    case 4: return I2CStatus::BusError;
    case 5: return I2CStatus::Timeout;
    default: return I2CStatus::UnknownError;
  }
}

bool Sht31Sensor::markFailure(I2CStatus status) {
  _lastI2CStatus = status;

  if (_consecutiveFailures < 255) {
    ++_consecutiveFailures;
  }

  if (_consecutiveFailures >= kFailureThreshold) {
    _healthy = false;
  }

  return false;
}
