#pragma once
#include <Arduino.h>

struct TelemetryPacket {
  uint32_t seq = 0;
  uint32_t uptimeMs = 0;

  uint8_t sensorFlags = 0;

  float windMps = -1.0f;
  float tempC = -999.0f;
  float humidityPct = -1.0f;

  float pm1_0 = NAN;
  float pm2_5 = NAN;
  float pm4_0 = NAN;
  float pm10  = NAN;

  double lat = 0.0;
  double lon = 0.0;
};
