#include "radio/TdmaRadioService.h"

#include "telemetry/BinaryPacket.h"

#include <string.h>

TdmaRadioService::TdmaRadioService(const TdmaConfig &cfg,
                                   TdmaClock &tdmaClock,
                                   TdmaTxQueue &queue,
                                   ITdmaRadioDriver &driver)
    : _cfg(cfg), _tdmaClock(tdmaClock), _queue(queue), _driver(driver) {}

bool TdmaRadioService::begin() {
  _error = TdmaRadioError::None;

  if (!_driver.begin()) {
    _state = TdmaRadioState::Error;
    _error = TdmaRadioError::BeginFailed;
    return false;
  }

  _state = TdmaRadioState::Ready;
  return true;
}

void TdmaRadioService::update() {
  if (_state != TdmaRadioState::Ready) {
    return;
  }

  checkIncomingTimeSync();
  drainTxQueue();
}

bool TdmaRadioService::enqueueTelemetry(const uint8_t *payload, uint8_t len) {
  if (_state != TdmaRadioState::Ready) {
    return false;
  }

  if (!_queue.enqueue(payload, len)) {
    _error = TdmaRadioError::EnqueueFailed;
    return false;
  }

  return true;
}

TdmaRadioState TdmaRadioService::state() const {
  return _state;
}

TdmaRadioError TdmaRadioService::error() const {
  return _error;
}

uint8_t TdmaRadioService::queuedCount() const {
  return _queue.count();
}

uint32_t TdmaRadioService::sentCount() const {
  return _sentCount;
}

uint32_t TdmaRadioService::failedSendCount() const {
  return _failedSendCount;
}

uint32_t TdmaRadioService::droppedOldestCount() const {
  return _queue.droppedOldestCount();
}

uint32_t TdmaRadioService::lastTxSlotIndex() const {
  return _lastTxSlotIndex;
}

void TdmaRadioService::drainTxQueue() {
  if (_queue.empty()) {
    return;
  }

  uint32_t slotIndex = 0;

  if (!_tdmaClock.myTurn(slotIndex)) {
    return;
  }

  if (slotIndex == _lastTxSlotIndex) {
    return;
  }

  _lastTxSlotIndex = slotIndex;

  uint8_t payload[TdmaConfig::MaxPayloadLen] = {};
  uint8_t len = 0;

  if (!_queue.dequeue(payload, len)) {
    return;
  }

  const bool ok = _driver.sendToWait(payload, len, _cfg.baseAddr);

  if (ok) {
    _sentCount++;
  } else {
    _failedSendCount++;
    _error = TdmaRadioError::SendFailed;

    // Match original behavior: failed LoRa send is dropped, not requeued.
  }
}

void TdmaRadioService::checkIncomingTimeSync() {
  while (_driver.available()) {
    ITdmaRadioDriver::ReceivedPacket packet;

    if (!_driver.receive(packet)) {
      return;
    }

    uint32_t sessionMs = 0;

    if (isTimeSyncPacket(packet, sessionMs)) {
      _tdmaClock.applySync(sessionMs);
    }
  }
}

bool TdmaRadioService::isTimeSyncPacket(
    const ITdmaRadioDriver::ReceivedPacket &packet,
    uint32_t &sessionMsOut) const {
  BinaryPacket::PktHeader hdr;
  BinaryPacket::TimeSyncPayload ts;

  if (!BinaryPacket::decodeTimeSync(packet.data, packet.len, hdr, ts)) {
    return false;
  }

  sessionMsOut = ts.session_time_ms;
  return true;
}
