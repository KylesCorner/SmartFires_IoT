#include "platform/AdafruitSht31Driver.h"

#include <Arduino.h>
#include <math.h>

namespace {

constexpr float kMinTempC = -40.0f;
constexpr float kMaxTempC = 125.0f;
constexpr float kMinHumidityPct = 0.0f;
constexpr float kMaxHumidityPct = 100.0f;

constexpr uint8_t kReadAttempts = 2;
constexpr uint16_t kRetryDelayMs = 5;

bool validTemperatureC(float value) {
  return isfinite(value) && value >= kMinTempC && value <= kMaxTempC;
}

bool validHumidityPct(float value) {
  return isfinite(value) && value >= kMinHumidityPct &&
         value <= kMaxHumidityPct;
}

} // namespace

bool AdafruitSht31Driver::begin(uint8_t address) {
  const bool ok = _sht31.begin(address);

  if (ok) {
    _sht31.heater(false);
  }

  return ok;
}

bool AdafruitSht31Driver::read(float &temperatureC, float &humidityPct) {
  for (uint8_t attempt = 0; attempt < kReadAttempts; ++attempt) {
    float temp = NAN;
    float humidity = NAN;

    const bool ok = _sht31.readBoth(&temp, &humidity);

    if (ok && validTemperatureC(temp) && validHumidityPct(humidity)) {
      temperatureC = temp;
      humidityPct = humidity;
      return true;
    }

    delay(kRetryDelayMs);
  }

  temperatureC = NAN;
  humidityPct = NAN;
  return false;
}

float AdafruitSht31Driver::readTemperatureC() {
  float temp = NAN;
  float humidity = NAN;

  if (!read(temp, humidity)) {
    return NAN;
  }

  return temp;
}

float AdafruitSht31Driver::readHumidityPct() {
  float temp = NAN;
  float humidity = NAN;

  if (!read(temp, humidity)) {
    return NAN;
  }

  return humidity;
}