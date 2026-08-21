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

  // Frames waiting to be handed to receive(), plus a record of which of them
  // the service chose to link-ACK. Whether a given inbound type gets an ack is
  // a wire-visible protocol decision — acking a command puts this node on the
  // air inside the base's own slot 0 — so it needs to be assertable, not a
  // silent no-op.
  static constexpr uint8_t kMaxInbox = 8;
  ReceivedPacket inbox[kMaxInbox] = {};
  uint8_t inboxCount = 0;
  uint8_t inboxHead = 0;

  uint8_t ackCount = 0;
  uint8_t lastAckTo = 0;
  uint8_t lastAckId = 0;

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

  bool available() override { return inboxHead < inboxCount; }

  bool sleep() override {
    sleepCount++;
    return true;
  }

  bool receive(ReceivedPacket &out, bool) override {
    if (inboxHead >= inboxCount) {
      return false;
    }
    out = inbox[inboxHead++];
    return true;
  }

  void acknowledge(uint8_t to, uint8_t id) override {
    ackCount++;
    lastAckTo = to;
    lastAckId = id;
  }

  bool healthy() const override { return healthyResult; }

  const SentFrame *last() const {
    return (sentCount == 0) ? nullptr : &sent[sentCount - 1];
  }

  void clearSent() { sentCount = 0; }

  // Queues a frame for the next receive(). `to` defaults to this node's unicast
  // address rather than kBroadcastAddress, since every type whose ack behaviour
  // is worth asserting is unicast.
  void pushInbound(const uint8_t *data, uint8_t len, uint8_t from, uint8_t to,
                   uint8_t id) {
    if (inboxCount >= kMaxInbox || !data) {
      return;
    }
    ReceivedPacket &p = inbox[inboxCount++];
    memcpy(p.data, data, len);
    p.len = len;
    p.from = from;
    p.to = to;
    p.id = id;
  }

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
