#pragma once
#include <Arduino.h>
#include "TelemetryPacket.h"

namespace TelemetryCodec {

inline bool encode(const TelemetryPacket& p, char* out, size_t outSize) {
  int n = snprintf(
    out,
    outSize,
    "TEL,%lu,%lu,%u,%u,%.3f,%.2f,%.2f,%d,%.6f,%.6f",
    (unsigned long)p.seq,
    (unsigned long)p.uptimeMs,
    (unsigned)p.sensorFlags,
    p.flameDetected ? 1u : 0u,
    p.windMps,
    p.tempC,
    p.humidityPct,
    (int)p.lidarCm,
    p.lat,
    p.lon
  );

  return n > 0 && (size_t)n < outSize;
}
inline bool parse(const char* line, TelemetryPacket& out) {
  if (!line) return false;
  if (strncmp(line, "TEL,", 4) != 0) return false;

  char buf[192];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // Split into exactly 11 fields, preserving empty ones:
  // 0 TEL
  // 1 seq
  // 2 uptimeMs
  // 3 sensorFlags
  // 4 flameDetected
  // 5 windMps
  // 6 tempC
  // 7 humidityPct
  // 8 lidarCm
  // 9 lat
  // 10 lon
 
  char* fields[11] = {0};
  size_t fieldCount = 0;

  char* p = buf;
  fields[fieldCount++] = p;

  while (*p && fieldCount < 11) {
    if (*p == ',') {
      *p = '\0';
      fields[fieldCount++] = p + 1;
    }
    ++p;
  }

  if (fieldCount != 11) return false;
  if (strcmp(fields[0], "TEL") != 0) return false;

  out.seq = strtoul(fields[1], nullptr, 10);
  out.uptimeMs = strtoul(fields[2], nullptr, 10);
  out.sensorFlags = (uint8_t)strtoul(fields[3], nullptr, 10);
  out.flameDetected = atoi(fields[4]) != 0;

  // Empty fields become 0.0 / 0, which is fine for now
  out.windMps = fields[5][0] ? atof(fields[5]) : 0.0f;
  out.tempC = fields[6][0] ? atof(fields[6]) : 0.0f;
  out.humidityPct = fields[7][0] ? atof(fields[7]) : 0.0f;
  out.lidarCm = fields[8][0] ? (int16_t)atoi(fields[8]) : 0;
  out.lat = fields[9][0] ? atof(fields[9]) : 0.0f;
  out.lon = fields[10][0] ? atof(fields[10]) : 0.0f;

  return true;
  }

}
