#pragma once
#include <Arduino.h>

struct TelemetryPacket {
  uint32_t seq = 0;
  uint32_t uptimeMs = 0;

  uint8_t sensorFlags = 0;
  bool flameDetected = false;

  float windMps = -1.0f;
  float tempC = -999.0f;
  float humidityPct = -1.0f;
  int16_t lidarCm = -1;

  double lat = 0.0;
  double lon = 0.0;
};
