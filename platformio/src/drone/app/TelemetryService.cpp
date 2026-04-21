#include "TelemetryService.h"

TelemetryPacket TelemetryService::build(uint32_t seq, uint32_t nowMs) const {
  TelemetryPacket p{};
  p.seq = seq;
  p.uptimeMs = nowMs;

  if (_ctx.wind.hasReading()) {
    p.sensorFlags |= TF_WIND;
    p.windMps = _ctx.wind.windMps();
  }

  if (_ctx.sht31.hasReading()) {
    p.sensorFlags |= TF_SHT31;
    p.tempC = _ctx.sht31.temperatureC();
    p.humidityPct = _ctx.sht31.humidityPct();
  }

  if (_ctx.gps.hasReading() && _ctx.gps.hasFix()) {
    p.sensorFlags |= TF_GPS;
    p.lat = _ctx.gps.latitudeDegrees();
    p.lon = _ctx.gps.longitudeDegrees();
  }

  if (_ctx.sps30.hasReading()) {
    p.sensorFlags |= TF_SPS30;
    const auto& r = _ctx.sps30.reading();
    p.pm1_0 = r.pm1_0;
    p.pm2_5 = r.pm2_5;
    p.pm4_0 = r.pm4_0;
    p.pm10  = r.pm10;
  }

  return p;
}
