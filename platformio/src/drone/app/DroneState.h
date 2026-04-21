#pragma once
#include <stdint.h>

enum class OledPage : uint8_t {
  Env = 0,
  Gps,
  Imu,
  Uart,
  PM,
  Count, // pages end here
  Lora,
  Empty
};

struct LinkState {
  uint32_t lastSendMs = 0;
  uint32_t seq = 0;
  uint32_t lastSentSeq = 0;
  uint32_t lastAckedSeq = 0;
  bool waitingForAck = false;
  uint32_t lastSendTimeMs = 0;
  bool bootMessagePrinted = false;
  uint32_t lastPrintedAck = 0;
};

struct UiState {
  OledPage currentPage = OledPage::Env;
  bool oledNeedsRefresh = true;
  uint32_t lastRenderMs = 0;
};

struct AppState {
  bool sensingEnabled = true;
  bool sensorsSleeping = false;
  bool lastSensorsSleeping = false;
  LinkState link;
  UiState ui;
};
