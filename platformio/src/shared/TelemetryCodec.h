#pragma once
#include <Arduino.h>
#include "TelemetryPacket.h"

// Legacy text/CSV codec — no longer used on the wire. Kept for reference only.
// Format: TEL,seq,uptimeMs,sensorFlags,windMps,tempC,humidityPct,lat,lon

namespace TelemetryCodec {

inline bool encode(const TelemetryPacket& p, char* out, size_t outSize) {
  int n = snprintf(
    out,
    outSize,
    "TEL,%lu,%lu,%u,%.3f,%.2f,%.2f,%.6f,%.6f",
    (unsigned long)p.seq,
    (unsigned long)p.uptimeMs,
    (unsigned)p.sensorFlags,
    p.windMps,
    p.tempC,
    p.humidityPct,
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

  // 0 TEL
  // 1 seq
  // 2 uptimeMs
  // 3 sensorFlags
  // 4 windMps
  // 5 tempC
  // 6 humidityPct
  // 7 lat
  // 8 lon

  char* fields[9] = {0};
  size_t fieldCount = 0;

  char* p = buf;
  fields[fieldCount++] = p;

  while (*p && fieldCount < 9) {
    if (*p == ',') {
      *p = '\0';
      fields[fieldCount++] = p + 1;
    }
    ++p;
  }

  if (fieldCount != 9) return false;
  if (strcmp(fields[0], "TEL") != 0) return false;

  out.seq        = strtoul(fields[1], nullptr, 10);
  out.uptimeMs   = strtoul(fields[2], nullptr, 10);
  out.sensorFlags = (uint8_t)strtoul(fields[3], nullptr, 10);
  out.windMps    = fields[4][0] ? atof(fields[4]) : 0.0f;
  out.tempC      = fields[5][0] ? atof(fields[5]) : 0.0f;
  out.humidityPct = fields[6][0] ? atof(fields[6]) : 0.0f;
  out.lat        = fields[7][0] ? atof(fields[7]) : 0.0;
  out.lon        = fields[8][0] ? atof(fields[8]) : 0.0;

  return true;
}

}
