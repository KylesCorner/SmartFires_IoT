#include "OledPageController.h"

#include <Arduino.h>
#include <string.h>

// OledPageController::OledPageController(DroneContext& ctx, AppState& state,
// IClock& clock)
//     : _ctx(ctx), _state(state), _clock(clock) {}
namespace {
constexpr uint32_t kOledRenderIntervalMs = 1000;
}
bool OledPageController::sensorsSleeping() const {
  return _state.sensorsSleeping;
}

bool OledPageController::sensingDisabled() const {
  return !_state.sensingEnabled;
}

bool OledPageController::warmingUp() const {
  return _state.wakeupSequenceActive;
}

const char *OledPageController::systemStateText() const {
  if (sensorsSleeping()) {
    return "SLEEP";
  }
  if (sensingDisabled()) {
    return "DISABLED";
  }
  if (warmingUp()) {
    return "WARMING";
  }

  if (_state.continousMode) {
    return "CONT";
  }

  return "ACTIVE";
}

const char *OledPageController::sensorPageHeader(const char *name) const {
  static char buf[24];

  if (sensorsSleeping()) {
    snprintf(buf, sizeof(buf), "%s [SLEEP]", name);
  } else if (sensingDisabled()) {
    snprintf(buf, sizeof(buf), "%s [DISABLED]", name);
  } else {
    // Later, swap this to page-specific warmup if needed.
    snprintf(buf, sizeof(buf), "%s", name);
  }

  return buf;
}

void OledPageController::render() {
  const uint32_t now = _clock.millis();

  // Only redraw if enough time has passed, unless a forced refresh was
  // requested.
  if (!_state.ui.oledNeedsRefresh &&
      (now - _state.ui.lastRenderMs) < kOledRenderIntervalMs) {
    return;
  }

  switch (_state.ui.currentPage) {

  case OledPage::Home:
    renderHome();
    break;

  case OledPage::Env:
    renderEnv();
    break;

  case OledPage::Gps:
    renderGps();
    break;

  case OledPage::Imu:
    renderImu();
    break;

  case OledPage::Uart:
    renderUart();
    break;

  case OledPage::PM:
    renderSps();
    break;

  default:
    _ctx.oled.printLine(0, "");
    _ctx.oled.printLine(1, "");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    break;
  }

  _state.ui.oledNeedsRefresh = false;
}
void OledPageController::renderHome() {
  _ctx.oled.printfLine(0, "State:%s", systemStateText());

  // UART state
  _ctx.oled.printfLine(1, "UART S:%lu A:%lu",
                       (unsigned long)_state.link.lastSentSeq,
                       (unsigned long)_state.link.lastAckedSeq);

  // GPS state
  if (sensorsSleeping()) {
    _ctx.oled.printLine(2, "GPS: SLEEP");
  } else if (sensingDisabled()) {
    _ctx.oled.printLine(2, "GPS: DISABLED");
  } else if (!_ctx.gps.hasReading()) {
    _ctx.oled.printLine(2, "GPS: WARMUP");
  } else if (!_ctx.gps.hasFix()) {
    _ctx.oled.printfLine(2, "GPS: NOFIX S:%u", _ctx.gps.satellites());
  } else {
    _ctx.oled.printfLine(2, "GPS: FIX S:%u", _ctx.gps.satellites());
  }

  // Link / duty / wake state
  if (sensorsSleeping()) {
    _ctx.oled.printLine(3, "Mode: Sleep");
  } else if (sensingDisabled()) {
    _ctx.oled.printLine(3, "Mode: Disabled");
  } else if (_state.link.waitingForAck) {
    _ctx.oled.printLine(3, "Mode: Waiting ACK");
  } else {
    _ctx.oled.printLine(3, "Mode: Running");
  }
}
void OledPageController::renderSps() {
  _ctx.oled.printLine(0, sensorPageHeader("SPS30"));

  if (sensorsSleeping()) {
    _ctx.oled.printLine(1, "Sleeping");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (sensingDisabled()) {
    _ctx.oled.printLine(1, "Disabled");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (!_ctx.sps30.hasReading()) {
    _ctx.oled.printLine(1, "Warmup / no data");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  const auto &r = _ctx.sps30.reading();

  _ctx.oled.printfLine(1, "PM1:%.1f PM2.5:%.1f", r.pm1_0, r.pm2_5);
  _ctx.oled.printfLine(2, "PM4:%.1f PM10:%.1f", r.pm4_0, r.pm10);
  _ctx.oled.printfLine(3, "TPS:%.2fum", r.typicalParticleSizeUm);
}
void OledPageController::renderEnv() {
  _ctx.oled.printLine(0, sensorPageHeader("Environment"));

  if (sensorsSleeping()) {
    _ctx.oled.printLine(1, "Wind: Sleep");
    _ctx.oled.printLine(2, "T/H: Sleep");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (sensingDisabled()) {
    _ctx.oled.printLine(1, "Wind: Disabled");
    _ctx.oled.printLine(2, "T/H: Disabled");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (_ctx.wind.hasReading()) {
    _ctx.oled.printfLine(1, "Wind: %.2f m/s", _ctx.wind.windMps());
  } else {
    _ctx.oled.printLine(1, "Wind: Warmup");
  }

  if (_ctx.sht31.hasReading()) {
    _ctx.oled.printfLine(2, "T:%.1fF H:%.1f%%", _ctx.sht31.temperatureF(),
                         _ctx.sht31.humidityPct());
  } else {
    _ctx.oled.printLine(2, "T/H: No data");
  }

  _ctx.oled.printLine(3, "");
}
void OledPageController::renderGps() {
  _ctx.oled.printLine(0, sensorPageHeader("GPS"));

  if (sensorsSleeping()) {
    _ctx.oled.printLine(1, "Sleeping");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (sensingDisabled()) {
    _ctx.oled.printLine(1, "Disabled");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (!_ctx.gps.hasReading()) {
    _ctx.oled.printLine(1, "Warmup / no data");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (!_ctx.gps.hasFix()) {
    _ctx.oled.printfLine(1, "No fix S:%u", _ctx.gps.satellites());
    _ctx.oled.printfLine(2, "Age:%lu ms",
                         (unsigned long)_ctx.gps.sentenceAgeMs());
    _ctx.oled.printLine(3, "Move outside");
    return;
  }

  _ctx.oled.printfLine(1, "Fix S:%u Alt:%.0fm", _ctx.gps.satellites(),
                       _ctx.gps.altitudeMeters());
  _ctx.oled.printfLine(2, "Lat:%.4f", _ctx.gps.latitudeDegrees());
  _ctx.oled.printfLine(3, "Lon:%.4f", _ctx.gps.longitudeDegrees());
}

void OledPageController::renderImu() {
  _ctx.oled.printLine(0, sensorPageHeader("IMU"));

  if (sensorsSleeping()) {
    _ctx.oled.printLine(1, "Sleeping");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (sensingDisabled()) {
    _ctx.oled.printLine(1, "Disabled");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  if (!_ctx.imu.hasReading()) {
    _ctx.oled.printLine(1, "Warmup / no data");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    return;
  }

  _ctx.oled.printfLine(1, "A:%.1f %.1f %.1f", _ctx.imu.ax_mg(),
                       _ctx.imu.ay_mg(), _ctx.imu.az_mg());
  _ctx.oled.printfLine(2, "G:%.1f %.1f %.1f", _ctx.imu.gx_dps(),
                       _ctx.imu.gy_dps(), _ctx.imu.gz_dps());
  _ctx.oled.printfLine(3, "M:%.1f %.1f %.1f", _ctx.imu.mx_uT(),
                       _ctx.imu.my_uT(), _ctx.imu.mz_uT());
}
void OledPageController::renderUart() {
  _ctx.oled.printfLine(0, "UART [%s]", systemStateText());

  _ctx.oled.printfLine(1, "Sent:%lu Ack:%lu",
                       (unsigned long)_state.link.lastSentSeq,
                       (unsigned long)_state.link.lastAckedSeq);

  _ctx.oled.printfLine(2, "WaitAck:%s",
                       _state.link.waitingForAck ? "YES" : "NO");

  _ctx.oled.printfLine(3, "UART:%s",
                       _ctx.bridge.hasError() ? _ctx.bridge.lastError() : "OK");
}
