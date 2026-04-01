#include "LidarLiteV3.h"
#include "esp32-hal.h"

bool LidarLiteV3::begin(TwoWire& wire) {
  _wire = &wire;
  _tBeginMs = millis();
  _hasReading = false;
  _healthyFlag = true;
  _consecutiveFailures = 0;
  _distanceCm = 0;
  _lastStatus = 0;
  _lastI2CStatus = I2CStatus::Ok;

  // Sensor is documented for 400 kHz I2C max/supported.
  // Safe to call if your board supports it.
  _wire->setClock(400000);

  return ping();
}

bool LidarLiteV3::ping() {
  if (_wire == nullptr) return false;

  _wire->beginTransmission(_addr);
  uint8_t rc = _wire->endTransmission(true);
  _lastI2CStatus = mapWireEndTx(rc);

  if (rc == 0) {
    recordSuccess();
    return true;
  }

  recordFailure();
  return false;
}

bool LidarLiteV3::ready() const {
  // Datasheet says measurements can begin roughly 22 ms after power-up.
  // We use 25 ms to keep it simple.
  return (millis() - _tBeginMs) >= 25;
}

uint32_t LidarLiteV3::ageMs() const {
  if (!_hasReading) return UINT32_MAX;
  return millis() - _lastSampleMs;
}

bool LidarLiteV3::sample() {
  uint16_t cm = 0;
  if (!readDistanceCm(cm)){
    _hasReading = false;
    _tBeginMs = millis();
    return false;
  }

  _distanceCm = cm;
  _lastSampleMs = millis();
  _hasReading = true;
  return true;
}

bool LidarLiteV3::readDistanceCm(uint16_t& outCm) {
  if (_wire == nullptr) return false;
  if (!ready()) return false;

  const uint8_t cmd = _useBiasCorrection ? CMD_MEASURE_WITH_BIAS : CMD_MEASURE_NO_BIAS;

  if (!writeReg8(REG_ACQ_COMMAND, cmd)) {
    return false;
  }

  if (!waitUntilReady()) {
    return false;
  }

  uint16_t raw = 0;
  if (!readReg16AutoInc(REG_DISTANCE_HIGH, raw)) {
    return false;
  }

  outCm = raw;
  recordSuccess();
  return true;
}

void LidarLiteV3::setAcquisitionCount(uint8_t val) {
  if (_wire == nullptr) return;
  writeReg8(REG_SIG_COUNT_VAL, val);
}

bool LidarLiteV3::writeReg8(uint8_t reg, uint8_t value) {
  if (_wire == nullptr) return false;

  _wire->beginTransmission(_addr);
  _wire->write(reg);
  _wire->write(value);
  uint8_t rc = _wire->endTransmission(true); // STOP required by this device
  _lastI2CStatus = mapWireEndTx(rc);

  if (rc != 0) {
    recordFailure();
    return false;
  }
  return true;
}

bool LidarLiteV3::readReg8(uint8_t reg, uint8_t& value) {
  if (_wire == nullptr) return false;

  // Important: no repeated-start on this sensor. Use STOP after register select.
  _wire->beginTransmission(_addr);
  _wire->write(reg);
  uint8_t rc = _wire->endTransmission(true);
  _lastI2CStatus = mapWireEndTx(rc);

  if (rc != 0) {
    recordFailure();
    return false;
  }

  const uint8_t n = _wire->requestFrom((int)_addr, 1, (int)true);
  if (n != 1 || !_wire->available()) {
    _lastI2CStatus = I2CStatus::Timeout;
    recordFailure();
    return false;
  }

  value = _wire->read();
  return true;
}

bool LidarLiteV3::readReg16AutoInc(uint8_t regHigh, uint16_t& value) {
  if (_wire == nullptr) return false;

  // Datasheet example uses 0x8F to read 0x0F then 0x10 via auto-increment.
  const uint8_t regWithAutoInc = regHigh | 0x80;

  _wire->beginTransmission(_addr);
  _wire->write(regWithAutoInc);
  uint8_t rc = _wire->endTransmission(true);
  _lastI2CStatus = mapWireEndTx(rc);

  if (rc != 0) {
    recordFailure();
    return false;
  }

  const uint8_t n = _wire->requestFrom((int)_addr, 2, (int)true);
  if (n != 2 || _wire->available() < 2) {
    _lastI2CStatus = I2CStatus::Timeout;
    recordFailure();
    return false;
  }

  const uint8_t hi = _wire->read();
  const uint8_t lo = _wire->read();
  value = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool LidarLiteV3::waitUntilReady() {
  const uint32_t t0 = millis();

  while ((millis() - t0) < _ioTimeoutMs) {
    uint8_t st = 0;
    if (!readReg8(REG_STATUS, st)) {
      return false;
    }

    _lastStatus = st;

    // Optional health/process flags from STATUS register.
    if (st & STATUS_PROCESS_ERRBIT) {
      recordFailure();
      return false;
    }

    if ((st & STATUS_BUSY_BIT) == 0) {
      // Health bit = 1 means reference and receiver bias operational
      // per datasheet. If it's low, we still return the sample path as failed.
      if ((st & STATUS_HEALTH_BIT) == 0) {
        recordFailure();
        return false;
      }
      return true;
    }

    delay(1);
  }

  _lastI2CStatus = I2CStatus::Timeout;
  recordFailure();
  return false;
}

void LidarLiteV3::recordFailure() {
  if (_consecutiveFailures < 255) {
    ++_consecutiveFailures;
  }
  if (_consecutiveFailures >= _maxFailures) {
    _healthyFlag = false;
  }
}

void LidarLiteV3::recordSuccess() {
  _consecutiveFailures = 0;
  _healthyFlag = true;
  _lastI2CStatus = I2CStatus::Ok;
}

I2CStatus LidarLiteV3::mapWireEndTx(uint8_t wireCode) const {
  switch (wireCode) {
    case 0: return I2CStatus::Ok;
    case 1: return I2CStatus::BufferTooSmall;
    case 2: return I2CStatus::NackOnAddress;
    case 3: return I2CStatus::NackOnData;
    case 4: return I2CStatus::BusError;
    default: return I2CStatus::UnknownError;
  }
}
