#pragma once
#include <Arduino.h>
#include <Wire.h>

#include "ISensor.h"
#include "II2CDevice.h"

// SparkFun library header (as used by their examples)
#include "ICM_20948.h"  // :contentReference[oaicite:3]{index=3}

/**
 * SparkFun Qwiic 9DoF IMU (ICM-20948) driver wrapper.
 *
 * Notes:
 * - SparkFun breakout supports two I2C addresses: 0x68 or 0x69 depending on AD0/ADR. :contentReference[oaicite:4]{index=4}
 * - SparkFun Example1 uses AD0_VAL=1 by default for their breakout. :contentReference[oaicite:5]{index=5}
 */
class Icm20948Imu : public ISensor, public II2CDevice {
public:
  // SparkFun library uses AD0_VAL (0 or 1) instead of passing the full address.
  // AD0_VAL=1 => 0x69, AD0_VAL=0 => 0x68. :contentReference[oaicite:6]{index=6}
  explicit Icm20948Imu(uint8_t ad0_val = 1, const char* sensorName = "ICM-20948")
    : _ad0_val(ad0_val), _name(sensorName) {}

  // ---- II2CDevice ----
  const char* name() const override { return _name; }

  uint8_t address() const override {
    // 0x68 when AD0 low, 0x69 when AD0 high. :contentReference[oaicite:7]{index=7}
    return (_ad0_val ? 0x69 : 0x68);
  }

  bool begin(TwoWire& wire = Wire) override {
    _wire = &wire;

    _hasReading = false;
    _healthy = true;

    // Typical for ICM-20948 over I2C: 400kHz is supported and used in SparkFun example. :contentReference[oaicite:8]{index=8}
    _wire->begin();
    _wire->setClock(400000);

    // SparkFun lib init loop pattern: call begin(), check status. :contentReference[oaicite:9]{index=9}
    _icm.begin(*_wire, _ad0_val);
    if (_icm.status != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    _tBeginMs = millis();
    _lastSampleMs = 0;
    _lastReadingMs = 0;
    return true;
  }

  bool ping() override {
    if (_wire == nullptr) return false;
    _wire->beginTransmission(address());
    return (_wire->endTransmission(true) == 0);
  }

  bool healthy() const override { return _healthy; }

  // ---- ISensor ----
  bool begin() override { return begin(Wire); }

  bool ready() const override {
    // If you want a warmup time, enforce it here.
    return true;
  }

  bool sample() override {
    if (!_healthy) return false;

    const uint32_t now = millis();
    if (now - _lastSampleMs < _minIntervalMs) return false;
    _lastSampleMs = now;

    // SparkFun pattern: check dataReady(), then getAGMT(). :contentReference[oaicite:10]{index=10}
    if (!_icm.dataReady()) return false;

    _icm.getAGMT();

    // Cache scaled values (library methods return scaled units; see their example). :contentReference[oaicite:11]{index=11}
    _ax_mg = _icm.accX();
    _ay_mg = _icm.accY();
    _az_mg = _icm.accZ();

    _gx_dps = _icm.gyrX();
    _gy_dps = _icm.gyrY();
    _gz_dps = _icm.gyrZ();

    _mx_uT = _icm.magX();
    _my_uT = _icm.magY();
    _mz_uT = _icm.magZ();

    _temp_C = _icm.temp();

    _hasReading = true;
    _lastReadingMs = now;
    return true;
  }

  uint32_t ageMs() const override {
    if (!_hasReading) return UINT32_MAX;
    return millis() - _lastReadingMs;
  }

  // ---- Reading getters ----
  bool hasReading() const { return _hasReading; }

  // Accel (mg)
  float ax_mg() const { return _ax_mg; }
  float ay_mg() const { return _ay_mg; }
  float az_mg() const { return _az_mg; }

  // Gyro (deg/s)
  float gx_dps() const { return _gx_dps; }
  float gy_dps() const { return _gy_dps; }
  float gz_dps() const { return _gz_dps; }

  // Mag (microtesla)
  float mx_uT() const { return _mx_uT; }
  float my_uT() const { return _my_uT; }
  float mz_uT() const { return _mz_uT; }

  // Temperature (C)
  float temp_C() const { return _temp_C; }

private:
  static constexpr uint32_t _minIntervalMs = 10; // ~100 Hz polling cap (adjust)

  TwoWire* _wire = nullptr;
  ICM_20948_I2C _icm; // SparkFun I2C driver object :contentReference[oaicite:12]{index=12}

  uint8_t _ad0_val = 1;
  const char* _name;

  bool _healthy = true;
  bool _hasReading = false;

  uint32_t _tBeginMs = 0;
  uint32_t _lastSampleMs = 0;
  uint32_t _lastReadingMs = 0;

  float _ax_mg = NAN, _ay_mg = NAN, _az_mg = NAN;
  float _gx_dps = NAN, _gy_dps = NAN, _gz_dps = NAN;
  float _mx_uT = NAN, _my_uT = NAN, _mz_uT = NAN;
  float _temp_C = NAN;
};