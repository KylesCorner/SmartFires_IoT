#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include "ICM_20948.h"
#include "II2CDevice.h"
#include "ISensor.h"

class Icm20948Imu : public ISensor, public II2CDevice {
public:
  enum class PowerMode : uint8_t { Normal = 0, DutyCycledAccel, Sleep };

public:
  explicit Icm20948Imu(uint8_t ad0_val = 1,
                       const char *sensorName = "ICM-20948")
      : _ad0_val(ad0_val), _name(sensorName) {}

  // ---- II2CDevice ----
  const char *name() const override { return _name; }

  uint8_t address() const override { return (_ad0_val ? 0x69 : 0x68); }

  bool begin(TwoWire &wire = Wire) override {
    _wire = &wire;

    _hasReading = false;
    _healthy = true;
    _powerMode = PowerMode::Normal;

    _wire->begin();
    _wire->setClock(400000);

    _icm.begin(*_wire, _ad0_val);
    if (_icm.status != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    _tBeginMs = millis();
    _lastSampleMs = 0;
    _lastReadingMs = 0;

    // Start in normal/full mode
    return configureNormalMode();
  }

  bool ping() override {
    if (_wire == nullptr)
      return false;
    _wire->beginTransmission(address());
    return (_wire->endTransmission(true) == 0);
  }

  bool healthy() const override { return _healthy; }

  // ---- ISensor ----
  bool begin() override { return begin(Wire); }

  bool ready() const override {
    return _healthy && (_powerMode != PowerMode::Sleep);
  }

  bool sample() override {
    if (!_healthy)
      return false;
    if (_powerMode == PowerMode::Sleep) {
      // Duty-cycled accel-only mode:
      _ax_mg = NAN;
      _ay_mg = NAN;
      _az_mg = NAN;
      // gyro and mag are intentionally not being maintained
      _gx_dps = NAN;
      _gy_dps = NAN;
      _gz_dps = NAN;

      _mx_uT = NAN;
      _my_uT = NAN;
      _mz_uT = NAN;

      // temp may not be meaningful / needed here
      _temp_C = NAN;
      return false;
    };

    const uint32_t now = millis();
    const uint32_t interval = (_powerMode == PowerMode::DutyCycledAccel)
                                  ? _dutyCycleIntervalMs
                                  : _normalIntervalMs;

    if (now - _lastSampleMs < interval)
      return false;
    _lastSampleMs = now;

    if (!_icm.dataReady())
      return false;

    _icm.getAGMT();

    // Always valid in both Normal and DutyCycledAccel
    _ax_mg = _icm.accX();
    _ay_mg = _icm.accY();
    _az_mg = _icm.accZ();

    if (_powerMode == PowerMode::Normal) {
      _gx_dps = _icm.gyrX();
      _gy_dps = _icm.gyrY();
      _gz_dps = _icm.gyrZ();

      _mx_uT = _icm.magX();
      _my_uT = _icm.magY();
      _mz_uT = _icm.magZ();

      _temp_C = _icm.temp();
    } else {
      // Duty-cycled accel-only mode:
      // gyro and mag are intentionally not being maintained
      _gx_dps = NAN;
      _gy_dps = NAN;
      _gz_dps = NAN;

      _mx_uT = NAN;
      _my_uT = NAN;
      _mz_uT = NAN;

      // temp may not be meaningful / needed here
      _temp_C = NAN;
    }

    _hasReading = true;
    _lastReadingMs = now;
    return true;
  }

  uint32_t ageMs() const override {
    if (!_hasReading)
      return UINT32_MAX;
    return millis() - _lastReadingMs;
  }

  // ---- Power control ----
  PowerMode powerMode() const { return _powerMode; }

  bool setNormalMode() {
    if (!_healthy)
      return false;
    return configureNormalMode();
  }

  bool setDutyCycledAccelMode() {
    if (!_healthy)
      return false;
    return configureDutyCycledAccelMode();
  }

  bool sleep() override {
    if (!_healthy)
      return false;
    if (_powerMode == PowerMode::Sleep) {
      return false;
    }

    if (_icm.sleep(true) != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    _powerMode = PowerMode::Sleep;
    _hasReading = false;
    return true;
  }

  bool wake() override {
    if (_icm.sleep(false) != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    delay(10);

    // Wake back into duty-cycled mode by default.
    // Change this to configureNormalMode() if you prefer.
    return configureNormalMode();
  }

  void setNormalIntervalMs(uint32_t ms) { _normalIntervalMs = ms; }

  void setDutyCycleIntervalMs(uint32_t ms) { _dutyCycleIntervalMs = ms; }

  // ---- Reading getters ----
  bool hasReading() const { return _hasReading; }

  float ax_mg() const { return _ax_mg; }
  float ay_mg() const { return _ay_mg; }
  float az_mg() const { return _az_mg; }

  float gx_dps() const { return _gx_dps; }
  float gy_dps() const { return _gy_dps; }
  float gz_dps() const { return _gz_dps; }

  float mx_uT() const { return _mx_uT; }
  float my_uT() const { return _my_uT; }
  float mz_uT() const { return _mz_uT; }

  float temp_C() const { return _temp_C; }

private:
  bool configureNormalMode() {

    if (_powerMode == PowerMode::Normal) {
      return false;
    }
    // Wake if needed
    if (_icm.sleep(false) != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    // Disable low-power cycling
    if (_icm.lowPower(false) != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    // Run accel + gyro continuously
    if (_icm.setSampleMode(ICM_20948_Internal_Acc,
                           ICM_20948_Sample_Mode_Continuous) !=
        ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    if (_icm.setSampleMode(ICM_20948_Internal_Gyr,
                           ICM_20948_Sample_Mode_Continuous) !=
        ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    _powerMode = PowerMode::Normal;
    _healthy = true;
    return true;
  }

  bool configureDutyCycledAccelMode() {
    // Wake if needed
    if (_icm.sleep(false) != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    // Put chip into low-power operation
    if (_icm.lowPower(true) != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    // Duty-cycle only accel
    if (_icm.setSampleMode(ICM_20948_Internal_Acc,
                           ICM_20948_Sample_Mode_Cycled) != ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    // Keep gyro out of cycled measurement
    if (_icm.setSampleMode(ICM_20948_Internal_Gyr,
                           ICM_20948_Sample_Mode_Continuous) !=
        ICM_20948_Stat_Ok) {
      _healthy = false;
      return false;
    }

    _powerMode = PowerMode::DutyCycledAccel;
    _healthy = true;
    return true;
  }

private:
  TwoWire *_wire = nullptr;
  ICM_20948_I2C _icm;

  uint8_t _ad0_val = 1;
  const char *_name = "ICM-20948";

  bool _healthy = true;
  bool _hasReading = false;

  PowerMode _powerMode = PowerMode::Normal;

  uint32_t _tBeginMs = 0;
  uint32_t _lastSampleMs = 0;
  uint32_t _lastReadingMs = 0;

  uint32_t _normalIntervalMs = 10;     // ~100 Hz cap
  uint32_t _dutyCycleIntervalMs = 250; // slower polling when in low-power mode

  float _ax_mg = NAN, _ay_mg = NAN, _az_mg = NAN;
  float _gx_dps = NAN, _gy_dps = NAN, _gz_dps = NAN;
  float _mx_uT = NAN, _my_uT = NAN, _mz_uT = NAN;
  float _temp_C = NAN;
};
