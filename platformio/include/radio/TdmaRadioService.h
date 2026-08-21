// ---
// description: Orchestrates TDMA-gated LoRa TX/RX, app-layer reliability retries, and command receipt.
// role: implementation
// ---
#pragma once

#include "config/NetworkConfig.h"
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
  struct ReceivedCommand {
    uint8_t data[TdmaConfig::MaxPayloadLen] = {};
    uint8_t len = 0;
    int8_t rssi = 0;
    uint8_t from = 0;
  };

  TdmaRadioService(const TdmaConfig &cfg,
                   TdmaClock &tdmaClock,
                   TdmaTxQueue &queue,
                   ITdmaRadioDriver &driver);

  bool begin();
  void update();

  bool sendAwakenHandshake(const uint8_t *payload, uint8_t len);
  bool sendImmediate(const uint8_t *payload, uint8_t len, bool requireLinkAck = true);
  bool enqueueTelemetry(const uint8_t *payload, uint8_t len);
  void flushTelemetryBuffers(const char *reason = "manual");
  bool takePendingCommand(ReceivedCommand &out);
  uint8_t nodeId() const;
  uint8_t numSlots() const;

  // Radio TX power passthrough, for the base-commanded dynamic TX power path
  // (PKT_CMD_SET_TX_POWER — see documentation/Pending_Plans/DYNAMIC_TX_POWER.md).
  // The service holds no opinion and keeps no copy: setTxPower() clamps to
  // NetworkConfig::kMinTxPowerDbm..kMaxTxPowerDbm and hands the result to the
  // driver, and txPowerDbm() reads straight back out of the driver, so the
  // value reported in STATUS can never drift from the value in the radio.
  //
  // Returns the power actually applied, which may be a clamped version of what
  // was asked for; the caller compares it against its request to detect that.
  int8_t setTxPower(int8_t dbm);
  int8_t txPowerDbm() const;

  TdmaRadioState state() const;
  TdmaRadioError error() const;

  uint8_t queuedCount() const;
  uint32_t sentCount() const;
  uint32_t retransmitCount() const;
  uint32_t failedSendCount() const;
  uint32_t droppedOldestCount() const;

  uint32_t lastTxSlotIndex() const;

  void setDutySleep(bool requested);

  // Called by the app right after the MCU returns from standby, with the
  // measured sleep duration. Every pending-window timestamp is in session-clock
  // terms, and the session clock now runs through standby (rtc-subsecond-sleep
  // Phase 2) — so without this the sleep counts as elapsed retry time and every
  // unacked entry is discarded as `max_age` on the first post-wake drain
  // (kTimedSleepMs 35 s > kReliabilityMaxAgeMs 30 s, so it is not a race). The
  // node cannot hear an ACK_SUMMARY with its radio off, so that interval is not
  // time the base was given to answer; sliding the timestamps forward excludes
  // it and leaves the entries retransmittable in the next active window.
  void notifyMcuStandby(uint32_t sleptMs);

  // Frame periods of retry hold applied once a PKT_WINDOW_BEGIN reaches the air.
  // The base defers a sleeping node's ACK_SUMMARY and releases it on the first
  // slot 0 after WINDOW_BEGIN, so the ack is at most one frame period behind the
  // marker; two frames leaves margin and matches the base's own
  // kAckSummaryNodeSilenceMs. Without the hold, an entry whose retry gate had
  // already opened would retransmit a full bundle in the same warmup the ack is
  // already on its way through — which is precisely the duplicate the window
  // markers exist to remove.
  static constexpr uint32_t kAckRoundTripFrames = 2;

private:

  bool _dutySleepRequested = false;

  // Compile-time capacity ceiling. Single source: NetworkConfig.h's
  // kReliabilityWindowHardCap, so the operating kReliabilityWindowDepth
  // value and this cap can never silently diverge.
  static constexpr uint8_t kMaxReliabilityWindow =
      NetworkConfig::kReliabilityWindowHardCap;

  struct PendingEntry {
    bool inUse = false;
    uint8_t seq = 0;
    uint8_t payload[TdmaConfig::MaxPayloadLen] = {};
    uint8_t len = 0;
    uint32_t firstSentMs = 0;
    uint32_t lastSentMs = 0;
    uint8_t attempts = 0;
    bool sentSuccessfully = false;
    bool ackGateOpened = false;  // set true when entry_age_ms >= retry_wait_ms first elapses
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
  uint32_t _lastAckSummarySessionMs = 0;
  bool _hasReceivedAckSummary = false;
  uint32_t _lastTxSlotIndex = 0xFFFFFFFFu;
  uint32_t _lastRetxAttemptSlotIndex = 0xFFFFFFFFu;
  uint32_t _lastFreshTelemetrySentMs = 0;
  uint32_t _lastRetxHealthLogMs = 0;
  bool _hasFreshTelemetrySent = false;
  bool _hasPendingCommand = false;
  ReceivedCommand _pendingCommand = {};
  bool _radioAsleep = false;

  void drainTxQueue();
  // Slides every pending entry's timestamps forward, delaying retry eligibility
  // by that much. Shared by notifyMcuStandby() (excluding radio-off time from
  // the retry clock) and the post-WINDOW_BEGIN hold (waiting out the ack the
  // base is about to release).
  void shiftPendingTimestamps(uint32_t shiftMs, const char *reason);
  void holdPendingRetriesForAckRoundTrip();
  void updateRxPower();
  void checkIncomingTimeSync();
  void rememberPendingCommand(const ITdmaRadioDriver::ReceivedPacket &packet);
  uint32_t computeRetryWaitMs() const;

  bool isTimeSyncPacket(const ITdmaRadioDriver::ReceivedPacket &packet,
                        uint32_t &sessionIdOut,
                        uint32_t &sessionMsOut,
                        uint8_t &assignedNodeIdOut) const;
  bool isAckSummaryPacket(const ITdmaRadioDriver::ReceivedPacket &packet,
                          BinaryPacket::AckSummaryPayload &ackOut) const;
  bool applyAssignedNodeId(uint8_t nodeId);

  bool isTelemetryPacketForNode(const uint8_t *payload, uint8_t len,
                                uint8_t &seqOut) const;
  void rememberSentTelemetry(const uint8_t *payload, uint8_t len,
                             bool sentSuccessfully);
  bool pickRetransmitCandidate(uint8_t *payloadOut, uint8_t &lenOut,
                               uint8_t &seqOut, uint8_t &pendingIndexOut);
  void markRetransmitSent(uint8_t pendingIndex);
  void applyAckSummary(const BinaryPacket::AckSummaryPayload &ack);
  void dropExpiredPending();
  void maybeLogRetransmitHealth();
};
