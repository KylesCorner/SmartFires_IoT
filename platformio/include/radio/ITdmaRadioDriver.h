// ---
// description: Hardware abstraction interface for the TDMA radio driver (send/receive/ack/sleep).
// role: interface
// docs: [tdma-protocol]
// ---
#pragma once

#include <stddef.h>
#include <stdint.h>

class ITdmaRadioDriver {
public:
  struct ReceivedPacket {
    uint8_t from = 0;
    uint8_t to = 0;
    uint8_t id = 0;
    uint8_t data[255] = {};
    uint8_t len = 0;
    int8_t rssi = 0;
  };

  // Matches RadioHead's RH_BROADCAST_ADDRESS (0xFF) without pulling a
  // RadioHead header into this radio-implementation-agnostic interface.
  // Callers use this to tell a broadcast ReceivedPacket::to apart from one
  // addressed directly to this node — see receive()/acknowledge() below.
  static constexpr uint8_t kBroadcastAddress = 0xFF;

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

  // autoAck defaults to true for RadioHead-library compatibility, but every
  // caller in this codebase (both node and base) now passes autoAck=false
  // and acks explicitly via acknowledge() below — see that method's comment
  // for why. Letting RadioHead's own RHReliableDatagram::recvfromAck() send
  // the ACK internally routes through its no-timeout waitPacketSent(); a
  // missed radio TX-done interrupt there hangs the whole board with no
  // recovery (confirmed in the field — see packet-reliability doc). Passing
  // autoAck=true is only still supported for driver-implementation symmetry
  // with RadioHead's own API, not because anything here should use it.
  virtual bool receive(ReceivedPacket &out, bool autoAck = true) = 0;

  // Sends a zero-payload RadioHead link-layer ACK for a packet received via
  // receive(out, /*autoAck=*/false). `from`/`id` must be the values that
  // receive() populated on `out` for that packet.
  //
  // This exists specifically to avoid RadioHead's own RHReliableDatagram::
  // acknowledge(), which is unreachable anyway (protected) and, more
  // importantly, blocks on the same no-timeout waitPacketSent() as above.
  // Implementations must build the ACK frame from RadioHead's *public*
  // primitives (setHeaderId/setHeaderFlags/sendto) and return without
  // waiting for TX completion — see RadioHeadTdmaDriver::acknowledge() for
  // the reference implementation. Never call this for a broadcast receipt
  // (ReceivedPacket::to == kBroadcastAddress): every node on the channel
  // would ack the same broadcast at once, colliding with each other.
  virtual void acknowledge(uint8_t from, uint8_t id) = 0;

  virtual bool healthy() const = 0;
};
