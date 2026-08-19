// ---
// description: Implements TdmaTxQueue's ring-buffer enqueue/dequeue with drop-oldest eviction.
// role: implementation
// ---
#include "radio/TdmaTxQueue.h"

#include <string.h>

TdmaTxQueue::TdmaTxQueue(uint8_t depth) {
  if (depth == 0) {
    _capacity = 1;
  } else if (depth > MaxDepth) {
    _capacity = MaxDepth;
  } else {
    _capacity = depth;
  }
}

bool TdmaTxQueue::enqueue(const uint8_t *payload, uint8_t len) {
  if (!payload || len == 0 || len > TdmaConfig::MaxPayloadLen) {
    return false;
  }

  if (_count >= _capacity) {
    _head = static_cast<uint8_t>((_head + 1) % _capacity);
    _count--;
    _droppedOldest++;
  }

  memcpy(_entries[_tail].payload, payload, len);
  _entries[_tail].len = len;

  _tail = static_cast<uint8_t>((_tail + 1) % _capacity);
  _count++;

  return true;
}

bool TdmaTxQueue::dequeue(uint8_t *payload, uint8_t &lenOut) {
  if (!payload || _count == 0) {
    lenOut = 0;
    return false;
  }

  lenOut = _entries[_head].len;
  memcpy(payload, _entries[_head].payload, lenOut);

  _head = static_cast<uint8_t>((_head + 1) % _capacity);
  _count--;

  return true;
}

bool TdmaTxQueue::peekPacketType(uint8_t &pktTypeOut) const {
  if (_count == 0 ||
      _entries[_head].len < sizeof(BinaryPacket::PktHeader)) {
    return false;
  }

  BinaryPacket::PktHeader hdr;
  memcpy(&hdr, _entries[_head].payload, sizeof(hdr));

  if (hdr.magic != BinaryPacket::PKT_MAGIC) {
    return false;
  }

  pktTypeOut = hdr.pkt_type;
  return true;
}

void TdmaTxQueue::clear() {
  _head = 0;
  _tail = 0;
  _count = 0;
}

bool TdmaTxQueue::empty() const {
  return _count == 0;
}

bool TdmaTxQueue::full() const {
  return _count >= _capacity;
}

uint8_t TdmaTxQueue::count() const {
  return _count;
}

uint8_t TdmaTxQueue::capacity() const {
  return _capacity;
}

uint32_t TdmaTxQueue::droppedOldestCount() const {
  return _droppedOldest;
}
