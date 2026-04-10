#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <new>
#include <math.h>
#include <SPI.h>

#include <Adafruit_SHT31.h>

#include "ISensor.h"
#include "II2CDevice.h"   // rename if your file has a different name

class Sht31Sensor final : public ISensor, public II2CDevice {
public:
  static constexpr uint8_t kDefaultAddress   = 0x44;
  static constexpr uint8_t kAlternateAddress = 0x45;
  static constexpr uint8_t kFailureThreshold = 3;

  explicit Sht31Sensor(uint8_t address = kDefaultAddress);

  ~Sht31Sensor() override;

  // ISensor
  const char* name() const override;
  bool begin() override;
  bool ready() const override;
  bool sample() override;
  uint32_t ageMs() const override;
  bool healthy() const override;

  // II2CDevice
  uint8_t address() const override;
  bool begin(TwoWire& wire) override;
  bool ping() override;

  // SHT31-specific accessors
  bool hasReading() const;
  float temperatureC() const;
  float temperatureF() const;
  float humidityPct() const;

  void setHeater(bool enabled);
  bool heaterEnabled() const;
  void reset();

  I2CStatus lastI2CStatus() const;

private:
  void constructDriver(TwoWire& wire);
  void destroyDriver();

  Adafruit_SHT31* driver();
  const Adafruit_SHT31* driver() const;

  static I2CStatus decodeWireStatus(uint8_t wireStatus);
  bool markFailure(I2CStatus status);

private:
  uint8_t _address;
  TwoWire* _wire;

  alignas(Adafruit_SHT31) uint8_t _driverStorage[sizeof(Adafruit_SHT31)];
  bool _driverConstructed;

  bool _begun;
  bool _healthy;
  bool _hasReading;

  uint8_t _consecutiveFailures;
  uint32_t _lastSampleMs;

  float _temperatureC;
  float _humidityPct;

  I2CStatus _lastI2CStatus;
};