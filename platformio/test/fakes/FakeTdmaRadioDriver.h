#pragma once

#include "radio/ITdmaRadioDriver.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class FakeTdmaRadioDriver final : public ITdmaRadioDriver {
public:
  static constexpr uint8_t MaxPackets = 8;
  static constexpr uint8_t MaxLen = 255;

  bool beginOk = true;
  bool sendOk = true;
  bool isHealthy = true;

  bool beginCalled = false;
  uint32_t sendCount = 0;

  uint8_t lastTo = 0;
  uint8_t lastSent[MaxLen] = {};
  uint8_t lastSentLen = 0;

  ReceivedPacket rx[MaxPackets] = {};
  uint8_t rxHead = 0;
  uint8_t rxTail = 0;
  uint8_t rxCount = 0;

  bool begin() override {
    beginCalled = true;
    isHealthy = beginOk;
    return beginOk;
  }

  bool send(const uint8_t *data, uint8_t len, uint8_t to) override {
    if (!sendOk || !data || len == 0) {
      return false;
    }

    lastTo = to;
    lastSentLen = len;
    memcpy(lastSent, data, len);
    sendCount++;

    return true;
  }

  bool sendToWait(const uint8_t *data, uint8_t len, uint8_t to) override {
    if (!sendOk || !data || len == 0) {
      return false;
    }

    lastTo = to;
    lastSentLen = len;
    memcpy(lastSent, data, len);
    sendCount++;

    return true;
  }

  bool available() override {
    return rxCount > 0;
  }

  bool receive(ReceivedPacket &out) override {
    if (rxCount == 0) {
      return false;
    }

    out = rx[rxHead];
    rxHead = static_cast<uint8_t>((rxHead + 1) % MaxPackets);
    rxCount--;

    return true;
  }

  bool healthy() const override {
    return isHealthy;
  }

  void queueRxString(const char *msg, uint8_t from = 0x01, int8_t rssi = -42) {
    if (!msg || rxCount >= MaxPackets) {
      return;
    }

    ReceivedPacket &p = rx[rxTail];

    p.from = from;
    p.rssi = rssi;
    p.len = static_cast<uint8_t>(strlen(msg));

    if (p.len > MaxLen) {
      p.len = MaxLen;
    }

    memcpy(p.data, msg, p.len);

    rxTail = static_cast<uint8_t>((rxTail + 1) % MaxPackets);
    rxCount++;
  }

  void queueRxBinary(const uint8_t *data, uint8_t len,
                     uint8_t from = 0x01, int8_t rssi = -42) {
    if (!data || len == 0 || rxCount >= MaxPackets) {
      return;
    }

    ReceivedPacket &p = rx[rxTail];

    p.from = from;
    p.rssi = rssi;
    p.len  = (len > MaxLen) ? MaxLen : len;

    memcpy(p.data, data, p.len);

    rxTail = static_cast<uint8_t>((rxTail + 1) % MaxPackets);
    rxCount++;
  }
};
