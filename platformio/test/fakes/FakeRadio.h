#pragma once

#include "interfaces/IRadio.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class FakeRadio final : public IRadio {
public:
  static constexpr size_t MaxPackets = 8;
  static constexpr size_t MaxPacketLen = 220;

  bool beginOk = true;
  bool sendOk = true;
  bool isHealthy = true;

  bool beginCalled = false;
  uint32_t sendCount = 0;

  char sentPackets[MaxPackets][MaxPacketLen] = {};
  size_t sentLens[MaxPackets] = {};

  char rxPackets[MaxPackets][MaxPacketLen] = {};
  size_t rxLens[MaxPackets] = {};
  size_t rxHead = 0;
  size_t rxTail = 0;

  bool begin() override {
    beginCalled = true;
    isHealthy = beginOk;
    return beginOk;
  }

  bool available() override {
    return rxHead != rxTail;
  }

  bool send(const uint8_t *data, size_t len) override {
    if (!sendOk || !data || len == 0) {
      return false;
    }

    const size_t index = sendCount % MaxPackets;
    const size_t copyLen = len < MaxPacketLen - 1 ? len : MaxPacketLen - 1;

    memcpy(sentPackets[index], data, copyLen);
    sentPackets[index][copyLen] = '\0';
    sentLens[index] = copyLen;

    sendCount++;
    return true;
  }

  size_t receive(uint8_t *out, size_t maxLen) override {
    if (!out || maxLen == 0 || !available()) {
      return 0;
    }

    const size_t index = rxHead % MaxPackets;
    const size_t len = rxLens[index];
    const size_t copyLen = len < maxLen ? len : maxLen;

    memcpy(out, rxPackets[index], copyLen);

    rxHead = (rxHead + 1) % MaxPackets;

    return copyLen;
  }

  bool healthy() const override {
    return isHealthy;
  }

  void queueRx(const char *msg) {
    if (!msg) {
      return;
    }

    const size_t index = rxTail % MaxPackets;
    const size_t len = strlen(msg);
    const size_t copyLen = len < MaxPacketLen - 1 ? len : MaxPacketLen - 1;

    memcpy(rxPackets[index], msg, copyLen);
    rxPackets[index][copyLen] = '\0';
    rxLens[index] = copyLen;

    rxTail = (rxTail + 1) % MaxPackets;
  }

  const char *lastSent() const {
    if (sendCount == 0) {
      return "";
    }

    const size_t index = (sendCount - 1) % MaxPackets;
    return sentPackets[index];
  }
};
