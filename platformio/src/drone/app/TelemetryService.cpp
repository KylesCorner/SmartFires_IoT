#include "TelemetryService.h"

TelemetryPacket TelemetryService::build(uint32_t seq, uint32_t nowMs) const {
  TelemetryPacket p{};
  p.seq = seq;
  p.uptimeMs = nowMs;

  // if (_ctx.flame.hasReading()) {
  //   p.sensorFlags |= TF_FLAME;
  //   p.flameDetected = _ctx.flame.detected();
  // }

  if (_ctx.wind.hasReading()) {
    p.sensorFlags |= TF_WIND;
    p.windMps = _ctx.wind.windMps();
  }

  if (_ctx.sht31.hasReading()) {
    p.sensorFlags |= TF_SHT31;
    p.tempC = _ctx.sht31.temperatureC();
    p.humidityPct = _ctx.sht31.humidityPct();
  }

  // if (_ctx.lidar.hasReading()) {
  //   p.sensorFlags |= TF_LIDAR;
  //   p.lidarCm = _ctx.lidar.distanceCm();
  // }

  if (_ctx.gps.hasReading() && _ctx.gps.hasFix()) {
    p.sensorFlags |= TF_GPS;
    p.lat = _ctx.gps.latitudeDegrees();
    p.lon = _ctx.gps.longitudeDegrees();
  }

  return p;
}
