#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>

#include "ISensor.h"
#include <SensirionUartSps30.h>
#include "PinMapping.h"

class Sps30UartSensor final : public ISensor {
public:
  struct Reading {
    float pm1_0 = NAN;
    float pm2_5 = NAN;
    float pm4_0 = NAN;
    float pm10 = NAN;

    float nc0p5 = NAN;
    float nc1p0 = NAN;
    float nc2p5 = NAN;
    float nc4p0 = NAN;
    float nc10p0 = NAN;

    float typicalParticleSizeUm = NAN;
  };

    struct Config {
    HardwareSerial* serial;
    uint8_t rxPin;
    uint8_t txPin;
    uint32_t baud;
    uint32_t warmupMs;
    uint32_t minSamplePeriodMs;
    uint8_t maxFailures;
    bool autoStartMeasurement;

    Config(HardwareSerial* serial_ = nullptr,
           uint8_t rxPin_ = PIN_SPS_RX,
           uint8_t txPin_ = PIN_SPS_TX,
           uint32_t baud_ = 115200,
           uint32_t warmupMs_ = 10000,
           uint32_t minSamplePeriodMs_ = 1000,
           uint8_t maxFailures_ = 5,
           bool autoStartMeasurement_ = true)
        : serial(serial_),
          rxPin(rxPin_),
          txPin(txPin_),
          baud(baud_),
          warmupMs(warmupMs_),
          minSamplePeriodMs(minSamplePeriodMs_),
          maxFailures(maxFailures_),
          autoStartMeasurement(autoStartMeasurement_) {}
  };

  Sps30UartSensor() = default;
  explicit Sps30UartSensor(const Config& cfg) : cfg_(cfg) {}

  const char* name() const override { return "SPS30-UART"; }

  bool begin() override;
  bool ready() const override;
  bool sample() override;
  bool hasReading() const override { return hasReading_; }
  uint32_t ageMs() const override;
  bool healthy() const override { return healthy_; }

  bool sleep() override;
  bool wake() override;

  const Reading& reading() const { return reading_; }

private:
  bool markFailure();
  void markSuccess();
  bool startMeasurement();

private:
  Config cfg_{};
  SensirionUartSps30 sensor_{};
  Reading reading_{};

  bool initialized_ = false;
  bool measuring_ = false;
  bool sleeping_ = false;
  bool hasReading_ = false;
  bool healthy_ = true;

  uint8_t consecutiveFailures_ = 0;

  uint32_t bootMs_ = 0;
  uint32_t lastSampleAttemptMs_ = 0;
  uint32_t lastReadingMs_ = 0;
};
