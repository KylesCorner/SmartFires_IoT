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
  if (strncmp(line, "TEL,", 4) != 0) return false;

  char buf[192];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* save = nullptr;
  char* tok = strtok_r(buf, ",", &save); // TEL
  if (!tok) return false;

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.seq = strtoul(tok, nullptr, 10);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.uptimeMs = strtoul(tok, nullptr, 10);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.sensorFlags = (uint8_t)strtoul(tok, nullptr, 10);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.flameDetected = atoi(tok) != 0;

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.windMps = atof(tok);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.tempC = atof(tok);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.humidityPct = atof(tok);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.lidarCm = (int16_t)atoi(tok);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.lat = atof(tok);

  tok = strtok_r(nullptr, ",", &save);
  if (!tok) return false;
  out.lon = atof(tok);

  return true;
}

} // namespace TelemetryCodec
