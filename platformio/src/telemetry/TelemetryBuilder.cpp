#include "telemetry/TelemetryBuilder.h"

#include <stdio.h>
#include <string.h>

TelemetryBuilder::TelemetryBuilder(const Config &cfg) : _cfg(cfg) {}

bool TelemetryBuilder::build(TelemetryFrame &frame,
                             ISensor **sensors,
                             size_t sensorCount,
                             const BatteryMonitor *battery) const {
  frame.clear();

  int n = snprintf(frame.payload,
                   TelemetryFrame::MaxLen,
                   "node=%u",
                   static_cast<unsigned>(_cfg.nodeId));

  if (n < 0 || static_cast<size_t>(n) >= TelemetryFrame::MaxLen) {
    frame.clear();
    return false;
  }

  frame.len = static_cast<size_t>(n);

  if (_cfg.includeBattery && battery) {
    char batteryBuf[96];

    if (battery->writeTelemetry(batteryBuf, sizeof(batteryBuf)) > 0) {
      if (!append(frame.payload, TelemetryFrame::MaxLen, frame.len, "|")) {
        return false;
      }

      if (!append(frame.payload, TelemetryFrame::MaxLen, frame.len,
                  batteryBuf)) {
        return false;
      }
    }
  }

  for (size_t i = 0; i < sensorCount; ++i) {
    ISensor *sensor = sensors[i];

    if (!sensor) {
      continue;
    }

    char sensorBuf[128];

    if (sensor->writeTelemetry(sensorBuf, sizeof(sensorBuf)) == 0) {
      continue;
    }

    if (!append(frame.payload, TelemetryFrame::MaxLen, frame.len, "|")) {
      return false;
    }

    if (!append(frame.payload, TelemetryFrame::MaxLen, frame.len, sensorBuf)) {
      return false;
    }
  }

  return frame.len > 0;
}

bool TelemetryBuilder::append(char *out,
                              size_t maxLen,
                              size_t &len,
                              const char *text) const {
  if (!out || !text || len >= maxLen) {
    return false;
  }

  const size_t textLen = strlen(text);

  if (len + textLen >= maxLen) {
    return false;
  }

  memcpy(out + len, text, textLen);
  len += textLen;
  out[len] = '\0';

  return true;
}
