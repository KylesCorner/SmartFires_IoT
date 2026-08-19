#pragma once

#include <stdint.h>
#include <string.h>

#include "config/NetworkConfig.h"
#include "radio/ITdmaRadioDriver.h"

// Records every frame handed to send()/sendToWait() so a test can inspect the
// exact bytes that would have gone on the air — the point being that
// TdmaRadioService mutates retransmit copies (PKT_FLAG_RETX + recomputed crc8),
// so "what was sent" and "what is stored in the pending window" are
// deliberately not the same bytes.
class FakeTdmaRadioDriver : public ITdmaRadioDriver {
public:
  struct SentFrame {
    uint8_t data[255] = {};
    uint8_t len = 0;
    uint8_t to = 0;
  };

  static constexpr uint8_t kMaxSent = 16;

  bool beginResult = true;
  bool sendResult = true;
  bool healthyResult = true;
  bool setTxPowerResult = true;

  SentFrame sent[kMaxSent] = {};
  uint8_t sentCount = 0;
  uint8_t sleepCount = 0;

  // Mirrors RadioHeadTdmaDriver's baseline default so a test that never
  // commands a power still reads back the same value real firmware would.
  int8_t txPower = NetworkConfig::kRadioTxPowerDbm;
  uint8_t setTxPowerCount = 0;

  bool begin() override { return beginResult; }

  bool send(const uint8_t *data, uint8_t len, uint8_t to) override {
    record(data, len, to);
    return sendResult;
  }

  bool sendToWait(const uint8_t *data, uint8_t len, uint8_t to) override {
    record(data, len, to);
    return sendResult;
  }

  bool setLocalAddress(uint8_t) override { return true; }

  bool setTxPower(int8_t dbm) override {
    txPower = dbm;
    setTxPowerCount++;
    return setTxPowerResult;
  }

  int8_t txPowerDbm() const override { return txPower; }

  bool available() override { return false; }

  bool sleep() override {
    sleepCount++;
    return true;
  }

  bool receive(ReceivedPacket &, bool) override { return false; }

  void acknowledge(uint8_t, uint8_t) override {}

  bool healthy() const override { return healthyResult; }

  const SentFrame *last() const {
    return (sentCount == 0) ? nullptr : &sent[sentCount - 1];
  }

  void clearSent() { sentCount = 0; }

private:
  void record(const uint8_t *data, uint8_t len, uint8_t to) {
    if (sentCount >= kMaxSent || !data) {
      return;
    }
    SentFrame &f = sent[sentCount++];
    memcpy(f.data, data, len);
    f.len = len;
    f.to = to;
  }
};
