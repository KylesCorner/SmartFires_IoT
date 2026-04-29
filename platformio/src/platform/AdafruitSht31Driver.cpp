#include "platform/AdafruitSht31Driver.h"

bool AdafruitSht31Driver::begin(uint8_t address) {
  return _sht31.begin(address);
}

float AdafruitSht31Driver::readTemperatureC() {
  return _sht31.readTemperature();
}

float AdafruitSht31Driver::readHumidityPct() {
  return _sht31.readHumidity();
}
