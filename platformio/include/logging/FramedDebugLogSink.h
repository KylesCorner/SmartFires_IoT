#pragma once

#include "telemetry/BinaryPacket.h"

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// Routes DebugLogger output (see logging/DebugLogger.h) through the same
// binary frame protocol used for telemetry, so @SFDBG lines reach the Jetson
// over the single USB link now shared with the base<->Jetson protocol (see
// documentation/Current_Architecture/UART_JETSON_BRIDGE.md). DebugLogger
// issues many Print::print() calls per logical line; this sink buffers them
// and flushes one PKT_DEBUG_LOG frame per line, on '\n'.
class FramedDebugLogSink : public Print {
public:
  // Max line length forwarded per frame. Bounded by encodeBaseFrame()'s
  // 254-byte lora_len ceiling minus the 4-byte PktHeader prefix — this sink
  // sits exactly at that limit. Lines longer than this are truncated, not
  // split across frames.
  static constexpr size_t kLineBufSize = 250;

  FramedDebugLogSink(Stream &jetsonUart, uint8_t baseAddr)
      : _jetsonUart(jetsonUart), _baseAddr(baseAddr) {}

  size_t write(uint8_t b) override {
    if (b == '\n') {
      flushLine();
      return 1;
    }
    if (_len < kLineBufSize) {
      _buf[_len++] = b;
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    for (size_t i = 0; i < size; ++i) {
      write(buffer[i]);
    }
    return size;
  }

private:
  void flushLine() {
    if (_len == 0) {
      return;
    }

    BinaryPacket::PktHeader hdr = {};
    hdr.magic = BinaryPacket::PKT_MAGIC;
    hdr.pkt_type = BinaryPacket::PKT_DEBUG_LOG;
    hdr.node_id = _baseAddr;
    hdr.seq = _seq++;

    uint8_t payload[sizeof(BinaryPacket::PktHeader) + kLineBufSize];
    memcpy(payload, &hdr, sizeof(hdr));
    memcpy(payload + sizeof(hdr), _buf, _len);
    const size_t payloadLen = sizeof(hdr) + _len;

    uint8_t frame[2 + 1 + 1 + 255 + 1] = {};
    const size_t frameLen = BinaryPacket::encodeBaseFrame(
        /*rssi=*/0, payload, payloadLen, frame, sizeof(frame));
    if (frameLen > 0) {
      _jetsonUart.write(frame, frameLen);
    }

    _len = 0;
  }

  Stream &_jetsonUart;
  uint8_t _baseAddr;
  uint8_t _seq = 0;
  uint8_t _buf[kLineBufSize];
  size_t _len = 0;
};
