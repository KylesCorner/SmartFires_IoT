// Pa1010dGpsSensor.h
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GPS.h>
#include <Adafruit_PMTK.h>

#include "ISensor.h"
#include "II2CDevice.h"

class Pa1010dGpsSensor final : public ISensor, public II2CDevice {
public:
  struct Reading {
    bool fix = false;
    uint8_t fixQuality = 0;
    uint8_t satellites = 0;

    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
    float altitudeM = 0.0f;
    float speedKnots = 0.0f;
    float courseDeg = 0.0f;
    float hdop = 0.0f;

    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t seconds = 0;
    uint16_t milliseconds = 0;

    uint8_t day = 0;
    uint8_t month = 0;
    uint8_t year = 0;   // 0..99 from GPS library
  };

  static constexpr uint8_t kDefaultAddress = GPS_DEFAULT_I2C_ADDR; // 0x10
  static constexpr uint8_t kDefaultCharsPerSample = 64;

  explicit Pa1010dGpsSensor(TwoWire& wire = Wire, uint8_t address = kDefaultAddress);

  // II2CDevice
  const char* name() const override { return "PA1010D Mini GPS"; }
  uint8_t address() const override { return _address; }
  bool begin(TwoWire& wire = Wire) override;
  bool ping() override;
  bool healthy() const override { return _healthy; }

  // ISensor
  bool begin() override { return begin(*_wire); }
  bool ready() const override { return _hasReading && _reading.fix; } // ready == currently has a fix
  bool sample() override;
  uint32_t ageMs() const override; // age of last valid fix, or UINT32_MAX if none yet
  bool hasReading() const override { return _hasReading;}

  // GPS-specific accessors
  bool hasFix() const { return _hasReading && _reading.fix; }

  const Reading& reading() const { return _reading; }

  float latitudeDegrees() const { return _reading.latitudeDeg; }
  float longitudeDegrees() const { return _reading.longitudeDeg; }
  float altitudeMeters() const { return _reading.altitudeM; }
  float speedKnots() const { return _reading.speedKnots; }
  float speedMps() const { return _reading.speedKnots * 0.514444f; }
  float courseDegrees() const { return _reading.courseDeg; }
  uint8_t satellites() const { return _reading.satellites; }

  uint32_t sentenceAgeMs() const;

private:
  bool configureGps_();
  void copyReading_();

  TwoWire* _wire;
  uint8_t _address;
  Adafruit_GPS _gps;

  Reading _reading;

  bool _begun = false;
  bool _healthy = false;
  bool _hasReading = false;

  uint8_t _consecutiveParseFailures = 0;

  uint32_t _lastByteMs = 0;
  uint32_t _lastSentenceMs = 0;
  uint32_t _lastFixMs = 0;
};