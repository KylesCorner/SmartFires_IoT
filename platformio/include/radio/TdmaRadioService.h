#pragma once

#include "radio/ITdmaRadioDriver.h"
#include "radio/TdmaClock.h"
#include "radio/TdmaConfig.h"
#include "radio/TdmaTxQueue.h"
#include "telemetry/BinaryPacket.h"

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

  bool sendAwakenHandshake(const uint8_t *payload, uint8_t len);
  bool enqueueTelemetry(const uint8_t *payload, uint8_t len);
  uint8_t nodeId() const;
  uint8_t numSlots() const;

  TdmaRadioState state() const;
  TdmaRadioError error() const;

  uint8_t queuedCount() const;
  uint32_t sentCount() const;
  uint32_t failedSendCount() const;
  uint32_t droppedOldestCount() const;

  uint32_t lastTxSlotIndex() const;

private:
  static constexpr uint8_t kMaxReliabilityWindow = 8;

  struct PendingEntry {
    bool inUse = false;
    uint8_t seq = 0;
    uint8_t payload[TdmaConfig::MaxPayloadLen] = {};
    uint8_t len = 0;
    uint32_t firstSentMs = 0;
    uint32_t lastSentMs = 0;
    uint8_t attempts = 0;
  };

  TdmaConfig _cfg;
  TdmaClock &_tdmaClock;
  TdmaTxQueue &_queue;
  ITdmaRadioDriver &_driver;

  PendingEntry _pending[kMaxReliabilityWindow] = {};
  uint8_t _pendingCount = 0;

  TdmaRadioState _state = TdmaRadioState::Off;
  TdmaRadioError _error = TdmaRadioError::None;

  uint32_t _sentCount = 0;
  uint32_t _enqueuedCount = 0;
  uint32_t _failedSendCount = 0;
  uint32_t _retransmitCount = 0;
  uint32_t _ackSummaryCount = 0;
  uint32_t _timeSyncCount = 0;
  uint32_t _pendingDropCount = 0;
  uint32_t _lastTxSlotIndex = 0xFFFFFFFFu;
  uint32_t _lastFreshTelemetrySentMs = 0;
  bool _hasFreshTelemetrySent = false;

  void drainTxQueue();
  void checkIncomingTimeSync();

  bool isTimeSyncPacket(const ITdmaRadioDriver::ReceivedPacket &packet,
                        uint32_t &sessionMsOut,
                        uint8_t &assignedNodeIdOut) const;
  bool isAckSummaryPacket(const ITdmaRadioDriver::ReceivedPacket &packet,
                          BinaryPacket::AckSummaryPayload &ackOut) const;
  bool applyAssignedNodeId(uint8_t nodeId);

  bool isTelemetryPacketForNode(const uint8_t *payload, uint8_t len,
                                uint8_t &seqOut) const;
  void rememberSentTelemetry(const uint8_t *payload, uint8_t len);
  bool pickRetransmitCandidate(uint8_t *payloadOut, uint8_t &lenOut,
                               uint8_t &seqOut, uint8_t &pendingIndexOut);
  void markRetransmitSent(uint8_t pendingIndex);
  void applyAckSummary(const BinaryPacket::AckSummaryPayload &ack);
  void dropExpiredPending();
};
