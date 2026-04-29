#pragma once

#include "radio/ITdmaRadioDriver.h"
#include "radio/TdmaClock.h"
#include "radio/TdmaConfig.h"
#include "radio/TdmaTxQueue.h"

#include <stddef.h>
#include <stdint.h>

enum class TdmaRadioState : uint8_t {
  Off,
  Ready,
  Error
};

enum class TdmaRadioError : uint8_t {
  None,
  BeginFailed,
  EnqueueFailed,
  SendFailed
};

class TdmaRadioService {
public:
  TdmaRadioService(const TdmaConfig &cfg,
                   TdmaClock &tdmaClock,
                   TdmaTxQueue &queue,
                   ITdmaRadioDriver &driver);

  bool begin();
  void update();

  bool enqueueTelemetry(const uint8_t *payload, uint8_t len);

  TdmaRadioState state() const;
  TdmaRadioError error() const;

  uint8_t queuedCount() const;
  uint32_t sentCount() const;
  uint32_t failedSendCount() const;
  uint32_t droppedOldestCount() const;

  uint32_t lastTxSlotIndex() const;

private:
  TdmaConfig _cfg;
  TdmaClock &_tdmaClock;
  TdmaTxQueue &_queue;
  ITdmaRadioDriver &_driver;

  TdmaRadioState _state = TdmaRadioState::Off;
  TdmaRadioError _error = TdmaRadioError::None;

  uint32_t _sentCount = 0;
  uint32_t _failedSendCount = 0;
  uint32_t _lastTxSlotIndex = 0xFFFFFFFFu;

  void drainTxQueue();
  void checkIncomingTimeSync();

  bool isTimeSyncPacket(const ITdmaRadioDriver::ReceivedPacket &packet,
                        uint32_t &sessionMsOut) const;
};
