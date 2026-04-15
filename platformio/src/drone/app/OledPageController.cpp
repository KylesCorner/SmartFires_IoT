#include "OledPageController.h"

#include <Arduino.h>
#include <string.h>

// OledPageController::OledPageController(DroneContext& ctx, AppState& state,
// IClock& clock)
//     : _ctx(ctx), _state(state), _clock(clock) {}
    namespace {
constexpr uint32_t kOledRenderIntervalMs = 250;
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
  case OledPage::Env:
    renderEnv();
    break;

  case OledPage::Gps:
    renderGps();
    break;

  case OledPage::Imu:
    renderImu();
    break;

  case OledPage::Lidar:
    renderLidar();
    break;

  case OledPage::Uart:
    renderUart();
    break;

  case OledPage::Lora:
    renderLora();
    break;

  default:
    _ctx.oled.printLine(0, "Empty Page");
    _ctx.oled.printLine(1, "");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
    break;
  }

  _state.ui.oledNeedsRefresh = false;
}

void OledPageController::renderEnv() {
  _ctx.oled.printLine(0, _state.sensingEnabled ? "Environment"
                                               : "Environment [PAUSED]");

  if (_ctx.wind.hasReading()) {
    _ctx.oled.printfLine(1, "Wind Speed:%.3f", _ctx.wind.windMps());
  } else {
    _ctx.oled.printLine(1, _state.sensingEnabled ? "Wind: Warming up"
                                                 : "Wind: paused");
  }

  if (_ctx.sht31.hasReading()) {
    _ctx.oled.printfLine(2, "T:%.1fF %.1fC H:%.1f%%", _ctx.sht31.temperatureF(),
                         _ctx.sht31.temperatureC(), _ctx.sht31.humidityPct());
  } else {
    _ctx.oled.printLine(2, _state.sensingEnabled ? "T/H:no reading"
                                                 : "T/H: paused");
  }

  if (_ctx.flame.hasReading()) {
    _ctx.oled.printfLine(3, "Flame:%d", _ctx.flame.analogRaw());
  } else {
    _ctx.oled.printLine(3, _state.sensingEnabled ? "No flame reading"
                                                 : "Flame: paused");
  }
}

void OledPageController::renderGps() {
  _ctx.oled.printLine(0, _state.sensingEnabled ? "GPS" : "GPS [PAUSED]");

  if (!_ctx.gps.hasReading()) {
    _ctx.oled.printLine(1, _state.sensingEnabled ? "No data" : "Paused");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
  } else if (!_ctx.gps.hasFix()) {
    _ctx.oled.printfLine(1, "No fix S:%u", _ctx.gps.satellites());
    _ctx.oled.printfLine(2, "Age:%lu ms",
                         (unsigned long)_ctx.gps.sentenceAgeMs());
    _ctx.oled.printLine(3, "Go outside");
  } else {
    _ctx.oled.printfLine(1, "S:%u Alt:%.0fm", _ctx.gps.satellites(),
                         _ctx.gps.altitudeMeters());
    _ctx.oled.printfLine(2, "Lat:%.4f", _ctx.gps.latitudeDegrees());
    _ctx.oled.printfLine(3, "Lon:%.4f", _ctx.gps.longitudeDegrees());
  }
}

void OledPageController::renderImu() {
  _ctx.oled.printLine(0, _state.sensingEnabled ? "IMU" : "IMU [PAUSED]");
  _ctx.oled.printLine(0, _state.sensorsSleeping? "IMU" : "IMU [SLEEP]");

  if (_ctx.imu.hasReading()) {
    _ctx.oled.printfLine(1, "A:%.1f %.1f %.1f", _ctx.imu.ax_mg(),
                         _ctx.imu.ay_mg(), _ctx.imu.az_mg());
    _ctx.oled.printfLine(2, "G:%.1f %.1f %.1f", _ctx.imu.gx_dps(),
                         _ctx.imu.gy_dps(), _ctx.imu.gz_dps());
    _ctx.oled.printfLine(3, "M:%.1f %.1f %.1f", _ctx.imu.mx_uT(),
                         _ctx.imu.my_uT(), _ctx.imu.mz_uT());
  } else {
    _ctx.oled.printLine(1, _state.sensingEnabled ? "No IMU reading" : "Paused");
    _ctx.oled.printLine(2, "");
    _ctx.oled.printLine(3, "");
  }
}

void OledPageController::renderLidar() {
  _ctx.oled.printLine(0, _state.sensingEnabled ? "LIDAR" : "LIDAR [PAUSED]");

  if (_ctx.lidar.hasReading()) {
    _ctx.oled.printfLine(1, "Dist: %d cm", _ctx.lidar.distanceCm());
  } else {
    _ctx.oled.printLine(1, _state.sensingEnabled ? "No readings" : "Paused");
  }

  _ctx.oled.printLine(2, "");
  _ctx.oled.printLine(3, "");
}

void OledPageController::renderUart() {
  _ctx.oled.printLine(0, "UART Connection");
  _ctx.oled.printfLine(1, "Sent:%lu Ack:%lu",
                       (unsigned long)_state.link.lastSentSeq,
                       (unsigned long)_state.link.lastAckedSeq);
  _ctx.oled.printfLine(2, "WaitAck:%s",
                       _state.link.waitingForAck ? "YES" : "NO");
  _ctx.oled.printfLine(3, "UART:%s",
                       _ctx.bridge.hasError() ? _ctx.bridge.lastError() : "OK");
}

void OledPageController::renderLora() {
  _ctx.oled.printLine(0, "LoRa Link");

  const bool bootSeen = _ctx.bridge.bootSeen();
  const bool linkAlive =
      bootSeen && ((_clock.millis() - _state.link.lastSendTimeMs) < 3000 ||
                   _state.link.waitingForAck == false);

  _ctx.oled.printfLine(1, "B:%s L:%s", bootSeen ? "Y" : "N",
                       linkAlive ? "OK" : "WAIT");

  _ctx.oled.printfLine(2, "S:%lu A:%lu", (unsigned long)_state.link.lastSentSeq,
                       (unsigned long)_state.link.lastAckedSeq);

  if (_ctx.bridge.hasRx()) {
    char rxPreview[22];
    strncpy(rxPreview, _ctx.bridge.lastRx(), sizeof(rxPreview) - 1);
    rxPreview[sizeof(rxPreview) - 1] = '\0';
    _ctx.oled.printfLine(3, "RX:%s", rxPreview);
  } else if (_state.link.waitingForAck) {
    _ctx.oled.printLine(3, "RX: waiting ack");
  } else {
    _ctx.oled.printLine(3, "RX: none");
  }
}
