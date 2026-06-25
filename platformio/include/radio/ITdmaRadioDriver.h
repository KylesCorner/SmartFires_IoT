// ---
// description: Hardware abstraction interface for the TDMA radio driver (send/receive/ack).
// role: interface
// ---
#pragma once

#include <stddef.h>
#include <stdint.h>

class ITdmaRadioDriver {
public:
  struct ReceivedPacket {
    uint8_t from = 0;
    uint8_t id = 0;
    uint8_t data[255] = {};
    uint8_t len = 0;
    int8_t rssi = 0;
  };

  virtual ~ITdmaRadioDriver() = default;

  virtual bool begin() = 0;
  virtual bool send(const uint8_t *data, uint8_t len, uint8_t to) = 0;
  virtual bool sendToWait(const uint8_t *data, uint8_t len, uint8_t to) = 0;
  virtual bool setLocalAddress(uint8_t address) = 0;

  virtual bool available() = 0;

  // Puts the radio into low-power sleep. Purely an idle-time optimization —
  // every other call on this interface (available(), send(), sendToWait())
  // re-arms whatever mode it needs on its own before acting, so callers never
  // need to "wake" the radio explicitly before using it again.
  virtual bool sleep() = 0;

  // autoAck=true (default) replies with a RadioHead link-layer ACK as soon as
  // a unicast packet is accepted — this is what every node-side receive call
  // wants, since every unicast packet the base sends to a node (TIME_SYNC,
  // ACK_SUMMARY, CMD_CALIBRATE/RESET) is sent with sendToWait() and blocks on
  // that ACK. Pass autoAck=false to receive without acking and decide later
  // via acknowledge() — used by the base, since most node->base traffic
  // (BUNDLE/STATUS) is sent fire-and-forget and never waits for one; ACKing
  // it anyway is just wasted airtime.
  virtual bool receive(ReceivedPacket &out, bool autoAck = true) = 0;

  // Sends a zero-payload RadioHead link-layer ACK for a packet received via
  // receive(out, /*autoAck=*/false). `from`/`id` must be the values that
  // receive() populated on `out` for that packet.
  virtual void acknowledge(uint8_t from, uint8_t id) = 0;

  virtual bool healthy() const = 0;
};
