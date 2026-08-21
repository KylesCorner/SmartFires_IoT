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
    // Signal-to-noise ratio in dB for this packet, as reported by the radio.
    //
    // Carried alongside rssi because RSSI alone cannot express LoRa link margin:
    // the modem demodulates *below* the noise floor (about -7.5 dB SNR at SF7),
    // so two packets at the same RSSI can be comfortably linked or one fade from
    // dropping depending on SNR. The base's TX power control loop needs the
    // margin, not the strength — see BaseConfig::kSnrDemodFloorDbX10.
    int8_t snr = 0;
  };

  // Matches RadioHead's RH_BROADCAST_ADDRESS (0xFF) without pulling a
  // RadioHead header into this radio-implementation-agnostic interface.
  // Callers use this to tell a broadcast ReceivedPacket::to apart from one
  // addressed directly to this node — see receive()/acknowledge() below.
  static constexpr uint8_t kBroadcastAddress = 0xFF;

  virtual ~ITdmaRadioDriver() = default;

  virtual bool begin() = 0;

  // Fire-and-forget send: returns as soon as RadioHead accepts the packet
  // for transmission, not once the caller's *previous* telemetry send
  // finished (that already happened, per this same requirement, before this
  // call was made). Implementations must still wait — with a *bounded*
  // timeout, never RadioHead's own no-arg waitPacketSent() — for this send's
  // own transmission to physically finish before returning, for the same
  // reason as ITdmaRadioDriver::acknowledge(): otherwise nothing prevents a
  // subsequent sleep() (or the next send()) from acting on the radio while
  // this transmission is still in flight. A timeout on that wait doesn't
  // necessarily mean the packet was lost — it's logged, not treated as
  // failure. The return value reflects only whether RadioHead accepted the
  // packet for transmission, not whether that bounded wait completed within
  // budget. See RadioHeadTdmaDriver::send() for the reference implementation.
  virtual bool send(const uint8_t *data, uint8_t len, uint8_t to) = 0;

  // See "Base Station Risk" in packet-reliability doc: implementations of
  // this one are expected to delegate to RadioHead's own RHReliableDatagram::
  // sendtoWait(), which blocks on its own no-timeout waitPacketSent() inside
  // a retry loop we have no way to interrupt from outside without patching
  // vendored code or reimplementing the whole retry/dedup protocol
  // ourselves. Unlike send()/acknowledge() above, there is currently no
  // bounded-wait guarantee here — a missed TX-done interrupt during any
  // retry attempt can still hang the caller indefinitely. A watchdog is the
  // intended mitigation for this call, not a call-site fix.
  virtual bool sendToWait(const uint8_t *data, uint8_t len, uint8_t to) = 0;

  virtual bool setLocalAddress(uint8_t address) = 0;

  // Changes the radio's transmit power, in dBm, for every subsequent send.
  // Callers are responsible for clamping to a sane range before calling
  // (NetworkConfig::kMinTxPowerDbm..kMaxTxPowerDbm) — this is a passthrough,
  // not a policy layer.
  //
  // Implementations must also make the new value survive a driver re-begin(),
  // since begin() re-applies the radio's whole configuration and would
  // otherwise silently revert a commanded power on the next radio reinit
  // (SmartFiresBaseApp's soft self-reset does exactly that). A genuine MCU
  // reboot still returns to the static baseline, because that reconstructs
  // Config from NetworkConfig — which is the intended fail-safe, not an
  // accident of this method.
  virtual bool setTxPower(int8_t dbm) = 0;

  // The value most recently applied by setTxPower(), or the configured
  // baseline if it has never been called. This is what the node reports in
  // StatusPayload::tx_power_dbm — ground truth from the radio's own
  // configuration rather than a separately-tracked copy that could drift.
  virtual int8_t txPowerDbm() const = 0;

  virtual bool available() = 0;

  // Puts the radio into low-power sleep. Purely an idle-time optimization —
  // every other call on this interface (available(), send(), sendToWait())
  // re-arms whatever mode it needs on its own before acting, so callers never
  // need to "wake" the radio explicitly before using it again.
  //
  // Implementations are not required to guard against an in-flight
  // transmission (RadioHead's own RH_RF95::sleep() doesn't). This is safe
  // only because acknowledge() below blocks (with a bound) until its own ACK
  // physically finishes sending before returning — if a future caller ever
  // fires off a transmission without waiting for it, sleep() being called
  // shortly after can silently abort it mid-send.
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
  // primitives (setHeaderId/setHeaderFlags/sendto) and wait for completion
  // only via a *bounded* wait (RHGenericDriver::waitPacketSent(timeout)) —
  // never the no-arg overload. An unbounded wait reintroduces the original
  // hang; no wait at all was tried first and found to have its own bug —
  // sleep() (see above) or another caller can abort the ACK mid-transmission
  // before it reaches the recipient, which surfaced in the field as the base
  // station retransmitting more than expected because nodes weren't
  // reliably acking. See RadioHeadTdmaDriver::acknowledge() for the
  // reference implementation and the packet-reliability doc for the full
  // history. Never call this for a broadcast receipt
  // (ReceivedPacket::to == kBroadcastAddress): every node on the channel
  // would ack the same broadcast at once, colliding with each other.
  virtual void acknowledge(uint8_t from, uint8_t id) = 0;

  virtual bool healthy() const = 0;
};
