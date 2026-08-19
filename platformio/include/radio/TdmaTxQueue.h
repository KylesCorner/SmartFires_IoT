// ---
// description: Fixed-depth drop-oldest ring buffer queuing outgoing LoRa payloads for TDMA transmission.
// role: implementation
// ---
#pragma once

#include "config/NetworkConfig.h"
#include "radio/TdmaConfig.h"
#include "telemetry/BinaryPacket.h"

#include <stddef.h>
#include <stdint.h>

class TdmaTxQueue {
public:
  struct Entry {
    uint8_t payload[TdmaConfig::MaxPayloadLen] = {};
    uint8_t len = 0;
  };

  // Compile-time capacity ceiling. Single source: NetworkConfig.h's
  // kQueueCapacityHardCap, so the operating kQueueDepth value and this cap
  // can never silently diverge.
  static constexpr uint8_t MaxDepth = NetworkConfig::kQueueCapacityHardCap;

  explicit TdmaTxQueue(uint8_t depth = 4);

  bool enqueue(const uint8_t *payload, uint8_t len);
  bool dequeue(uint8_t *payload, uint8_t &lenOut);
  // Packet type of the frame at the head of the queue, without removing it.
  // Returns false when the queue is empty or the head is too short to hold a
  // PktHeader. Lets the drain loop decide between the queue and a retransmit on
  // the basis of what is actually waiting.
  bool peekPacketType(uint8_t &pktTypeOut) const;
  void clear();

  bool empty() const;
  bool full() const;
  uint8_t count() const;
  uint8_t capacity() const;

  uint32_t droppedOldestCount() const;

private:
  Entry _entries[MaxDepth];

  uint8_t _capacity = 4;
  uint8_t _head = 0;
  uint8_t _tail = 0;
  uint8_t _count = 0;

  uint32_t _droppedOldest = 0;
};
