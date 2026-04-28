#include "Sps30UartSensor.h"
#include "USBCDC.h"

bool Sps30UartSensor::markFailure() {
  if (consecutiveFailures_ < 255) {
    ++consecutiveFailures_;
  }
  if (consecutiveFailures_ >= cfg_.maxFailures) {
    healthy_ = false;
  }
  return false;
}

void Sps30UartSensor::markSuccess() {
  consecutiveFailures_ = 0;
  healthy_ = true;
}

bool Sps30UartSensor::startMeasurement() {
  int16_t err =
      sensor_.startMeasurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);
  if (err != 0) {
    Serial.print("[SPS30-UART] startMeasurement err=");
    Serial.println(err);
    measuring_ = false;
    return markFailure();
  }

  measuring_ = true;
  sleeping_ = false;
  markSuccess();
  return true;
}

bool Sps30UartSensor::begin() {
  if (cfg_.serial == nullptr) {
    Serial.println("[SPS30-UART] serial pointer is null");
    return false;
  }

  bootMs_ = millis();
  lastSampleAttemptMs_ = 0;
  lastReadingMs_ = 0;
  hasReading_ = false;
  sleeping_ = false;
  measuring_ = false;
  consecutiveFailures_ = 0;
  healthy_ = true;

  cfg_.serial->begin(cfg_.baud, SERIAL_8N1, cfg_.rxPin, cfg_.txPin);
  delay(50);

  sensor_.begin(*cfg_.serial);

  initialized_ = true;
  markSuccess();

  if (cfg_.autoStartMeasurement) {
    return startMeasurement();
  }

  return true;
}

bool Sps30UartSensor::ready() const {
  if (!initialized_ || sleeping_ || !measuring_) {
    return false;
  }

  return (millis() - bootMs_) >= cfg_.warmupMs;
}

bool Sps30UartSensor::sample() {
  if (!initialized_) {
    Serial.println("[SPS30-UART] sample: not initialized");
    return false;
  }
  if (sleeping_) {
    Serial.println("[SPS30-UART] sample: sleeping");
    return false;
  }
  if (!measuring_) {
    Serial.println("[SPS30-UART] sample: not measuring");
    return false;
  }

  const uint32_t now = millis();
  if ((now - lastSampleAttemptMs_) < cfg_.minSamplePeriodMs) {
    return false;
  }
  lastSampleAttemptMs_ = now;

  if (!ready()) {
    Serial.println("[SPS30-UART] sample: not ready yet");
    return false;
  }

  int16_t err = sensor_.readMeasurementValuesFloat(
      reading_.pm1_0, reading_.pm2_5, reading_.pm4_0, reading_.pm10,
      reading_.nc0p5, reading_.nc1p0, reading_.nc2p5, reading_.nc4p0,
      reading_.nc10p0, reading_.typicalParticleSizeUm);

  if (err != 0) {
    Serial.print("[SPS30-UART] readMeasurementValuesFloat err=");
    Serial.println(err);
    return markFailure();
  }

  hasReading_ = true;
  lastReadingMs_ = now;
  markSuccess();
  return true;
}

uint32_t Sps30UartSensor::ageMs() const {
  if (!hasReading_) {
    return UINT32_MAX;
  }
  return millis() - lastReadingMs_;
}

bool Sps30UartSensor::sleep() {
  if (!initialized_) {
    return false;
  }

  // If we are actively measuring, stop first.
  if (measuring_) {
    int16_t err = sensor_.stopMeasurement();
    if (err != 0) {
      Serial.print("[SPS30-UART] stopMeasurement err=");
      Serial.println(err);
      return markFailure();
    }
    measuring_ = false;

    // Give the sensor a brief moment to settle into idle.
    delay(20);
  }

  int16_t err = sensor_.sleep();
  if (err != 0) {
    Serial.print("[SPS30-UART] sleep err=");
    Serial.println(err);
    return markFailure();
  }

  sleeping_ = true;
  markSuccess();
  return true;
}
bool Sps30UartSensor::wake() {
  if (!initialized_) {
    return false;
  }

  if (!sleeping_) {
    return true;
  }

  int16_t err = sensor_.wakeUpSequence();
  if (err != 0) {
    Serial.print("[SPS30-UART] wakeUpSequence err=");
    Serial.println(err);
    return markFailure();
  }

  sleeping_ = false;
  hasReading_ = false;
  bootMs_ = millis();
  markSuccess();

  delay(50);

  if (cfg_.autoStartMeasurement) {
    return startMeasurement();
  }

  return true;
}
