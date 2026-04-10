#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "II2CDevice.h"
#include "ISensor.h"

class LidarLiteV3 : public II2CDevice, public ISensor {
public:
  static constexpr uint8_t kDefaultAddress = 0x62;

  explicit LidarLiteV3(
      uint8_t addr = kDefaultAddress,
      const char* sensorName = "LIDAR-Lite v3",
      uint32_t ioTimeoutMs = 50,
      uint8_t maxFailuresBeforeUnhealthy = 3)
    : _addr(addr),
      _name(sensorName),
      _ioTimeoutMs(ioTimeoutMs),
      _maxFailures(maxFailuresBeforeUnhealthy) {}

  // ---- II2CDevice ----
  const char* name() const override { return _name; }
  uint8_t address() const override { return _addr; }
  bool begin(TwoWire& wire = Wire) override;
  bool ping() override;
  bool healthy() const override { return _healthyFlag; }

  // ---- ISensor ----
  bool begin() override { return begin(Wire); }
  bool ready() const override;
  bool sample() override;
  bool hasReading() const override { return _hasReading; }
  uint32_t ageMs() const override;

  // ---- LIDAR-specific API ----
  bool readDistanceCm(uint16_t& outCm);
  uint16_t distanceCm() const { return _distanceCm; }
  uint8_t statusReg() const { return _lastStatus; }
  I2CStatus lastI2CStatus() const { return _lastI2CStatus; }

  // Optional tuning
  void setTimeoutMs(uint32_t ms) { _ioTimeoutMs = ms; }
  void setAcquisitionCount(uint8_t val);   // register 0x02
  void setMeasurementBiasCorrection(bool enable) { _useBiasCorrection = enable; }

private:
  // Registers
  static constexpr uint8_t REG_ACQ_COMMAND    = 0x00;
  static constexpr uint8_t REG_STATUS         = 0x01;
  static constexpr uint8_t REG_SIG_COUNT_VAL  = 0x02;
  static constexpr uint8_t REG_DISTANCE_HIGH  = 0x0F;
  static constexpr uint8_t REG_DISTANCE_LOW   = 0x10;

  // Commands
  static constexpr uint8_t CMD_MEASURE_NO_BIAS   = 0x03;
  static constexpr uint8_t CMD_MEASURE_WITH_BIAS = 0x04;

  // STATUS bits
  static constexpr uint8_t STATUS_BUSY_BIT       = 0x01; // bit 0
  static constexpr uint8_t STATUS_HEALTH_BIT     = 0x20; // bit 5
  static constexpr uint8_t STATUS_PROCESS_ERRBIT = 0x40; // bit 6

  bool writeReg8(uint8_t reg, uint8_t value);
  bool readReg8(uint8_t reg, uint8_t& value);
  bool readReg16AutoInc(uint8_t regHigh, uint16_t& value);
  bool waitUntilReady();
  void recordFailure();
  void recordSuccess();
  I2CStatus mapWireEndTx(uint8_t wireCode) const;

private:
  TwoWire* _wire = nullptr;
  uint8_t _addr;
  const char* _name;

  uint32_t _ioTimeoutMs = 50;
  uint8_t _maxFailures = 3;
  bool _useBiasCorrection = true;

  bool _hasReading = false;
  bool _healthyFlag = true;
  uint8_t _consecutiveFailures = 0;

  uint16_t _distanceCm = 0;
  uint8_t _lastStatus = 0;
  I2CStatus _lastI2CStatus = I2CStatus::Ok;

  uint32_t _tBeginMs = 0;
  uint32_t _lastSampleMs = 0;
};
