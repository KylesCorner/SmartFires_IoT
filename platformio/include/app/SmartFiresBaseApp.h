// ---
// description: Top-level base-station application class — LoRa RX/TX, node assignment, ACK_SUMMARY tracking, and Jetson UART bridging, gated by the base's own reserved TDMA slot.
// role: implementation
// ---
#pragma once

#include "config/BaseConfig.h"
#include "interfaces/IClock.h"
#include "radio/ITdmaRadioDriver.h"
#include "radio/TdmaClock.h"
#include "radio/TdmaConfig.h"
#include "telemetry/BinaryPacket.h"

#include <stddef.h>
#include <stdint.h>

class Stream;
class Print;

class SmartFiresBaseApp {
public:
  struct Config {
    uint8_t baseAddr = BaseConfig::kBaseAddr;
    uint8_t timeSyncBroadcastAddr = BaseConfig::kTimeSyncBroadcastAddr;
    uint32_t uartBaud = BaseConfig::kUartBaud;
    uint32_t ackSummaryMinIntervalMs = BaseConfig::kAckSummaryMinIntervalMs;
    uint8_t tdmaNumSlots = BaseConfig::kTdmaNumSlots;
    uint32_t tdmaSlotWidthMs = BaseConfig::kTdmaSlotWidthMs;
    uint32_t tdmaGuardMs = BaseConfig::kTdmaGuardMs;

    // Thin wrapper: every field comes from config/BaseConfig.h, which in
    // turn shares its TDMA geometry with config/NetworkConfig.h so the base
    // and node builds can no longer drift apart on NUM_SLOTS/slotWidthMs/
    // guardMs the way they used to (this factory used to hardcode its own
    // independent 4/900/20 defaults).
    static Config baseCfg(uint8_t baseAddr_ = BaseConfig::kBaseAddr) {
      Config cfg;
      cfg.baseAddr = baseAddr_;
      return cfg;
    }
  };

  SmartFiresBaseApp(const Config &cfg,
                    IClock &clock,
                    ITdmaRadioDriver &radio,
                    Stream &jetsonUart,
                    Print &debugUart);

  bool begin();
  void update();

private:
  static constexpr uint8_t kTotalEntities = BaseConfig::kTotalEntities;
  static constexpr uint8_t kMaxAssignedNodes = BaseConfig::kMaxAssignedNodes;
  static constexpr uint8_t kFirstNodeId = BaseConfig::kFirstNodeId;
  static constexpr uint8_t kMaxAckTrackedNodes = BaseConfig::kMaxAckTrackedNodes;

  struct NodeAssignment {
    bool inUse = false;
    uint32_t uidHash = 0;
    uint8_t nodeId = 0;

    // Set when an AWAKEN arrives outside the base's reserved TDMA window
    // (slot 0) — the direct TIME_SYNC reply is deferred and flushed by
    // sendPendingDirectTimeSync() the next time the window opens, instead of
    // replying immediately at an arbitrary phase that could land inside a
    // real node's TX slot.
    bool pendingDirectSync = false;
    uint8_t pendingRadioAddr = 0;
    uint8_t pendingTriggerSeq = 0;
  };

  // CMD_CALIBRATE/CMD_RESET forwards from the Jetson are deferred the same
  // way — encoded immediately (so the original UART payload doesn't need to
  // be retained) but only transmitted once the base's window opens.
  static constexpr size_t kPendingCommandPayloadSize =
      BinaryPacket::kCmdCalibrateLoRaSize > BinaryPacket::kCmdResetLoRaSize
          ? BinaryPacket::kCmdCalibrateLoRaSize
          : BinaryPacket::kCmdResetLoRaSize;

  struct PendingCommand {
    bool inUse = false;
    uint8_t targetNodeId = 0;
    uint8_t payload[kPendingCommandPayloadSize] = {};
    uint8_t len = 0;
  };

  static constexpr uint8_t kMaxPendingCommands = 4;

  struct UartRxState {
    enum class Stage : uint8_t {
      WaitM0,
      WaitM1,
      WaitLen,
      ReadData,
      WaitCrc
    };

    Stage stage = Stage::WaitM0;
    uint8_t len = 0;
    uint8_t data[255] = {};
    uint8_t dataPos = 0;
    uint8_t crc = 0;
  };

  struct AckTracker {
    bool inUse = false;
    bool initialized = false;
    uint8_t nodeId = 0;
    uint8_t ackBaseSeq = 0;
    uint16_t ackMask = 0;
    bool dirty = false;
    uint8_t dirtyTriggerSeq = 0;
    bool lastSentInitialized = false;
    uint8_t lastSentAckBaseSeq = 0;
    uint16_t lastSentAckMask = 0;
    bool receiptWindowInitialized = false;
    uint8_t receiptWindowStartSeq = 0;
    uint32_t receiptWindowMask = 0;
  };

  static constexpr uint32_t kHealthLogPeriodMs = BaseConfig::kHealthLogPeriodMs;
  static constexpr uint32_t kPeriodicTimeSyncMs = BaseConfig::kPeriodicTimeSyncMs;

  Config _cfg;
  IClock &_clock;
  ITdmaRadioDriver &_radio;
  Stream &_jetsonUart;
  Print &_debugUart;

  // The base's own reserved-slot timing. nodeId=1 is the permanently
  // reserved identity that maps to slot 0 via TdmaClock's
  // slot=(nodeId-1)%numSlots — see config/BaseConfig.h's kFirstNodeId=2
  // (real nodes start at 2, so node 1/slot 0 is never assigned to one).
  // Self-clocking: applySync() is called every update() tick from the
  // base's own currentTimeSyncPayload(), so it never depends on receiving
  // anything over the radio.
  TdmaClock _baseTdmaClock;

  UartRxState _uartRx;
  bool _initialized = false;

  uint32_t _lastHealthLogMs = 0;
  uint32_t _rxForwardCount = 0;
  uint32_t _cmdForwardCount = 0;
  uint32_t _uartFrameErrorCount = 0;
  uint32_t _uartByteRxCount = 0;
  uint32_t _awakenRxCount = 0;
  uint32_t _bundleRxCount = 0;
  uint32_t _statusRxCount = 0;
  uint32_t _fullStateRxCount = 0;
  uint32_t _cmdAckRxCount = 0;
  uint32_t _rawRxCount = 0;
  uint32_t _timeSyncTxCount = 0;
  uint32_t _ackTxCount = 0;
  uint32_t _radioReceiveFailCount = 0;
  uint32_t _lastRxMs = 0;
  uint32_t _lastPeriodicTimeSyncMs = 0;
  uint32_t _lastAckSummaryFlushMs = 0;
  uint32_t _sessionId = 0;
  uint8_t _timeSyncSeq = 0;
  bool _hasJetsonTime = false;
  uint32_t _jetsonSessionId = 0;
  uint32_t _jetsonSessionMsAtUpdate = 0;
  uint32_t _localMsAtJetsonUpdate = 0;
  uint8_t _ackSummarySeq = 0;
  uint8_t _nextAckTrackerFlushIndex = 0;
  uint32_t _lastAckSummaryFlushSlotIndex = 0xFFFFFFFFu;
  NodeAssignment _nodeAssignments[kMaxAssignedNodes] = {};
  AckTracker _ackTrackers[kMaxAckTrackedNodes] = {};
  PendingCommand _pendingCommands[kMaxPendingCommands] = {};

  void processIncomingLoRa();
  void processIncomingJetsonUart();
  bool handleJetsonCommandPayload(const uint8_t *payload, uint8_t len);
  bool sendDirectTimeSync(uint8_t radioAddr, uint8_t nodeId, const char *reason,
                          uint8_t triggerSeq = 0);
  NodeAssignment *findOrCreateNodeAssignment(uint32_t uidHash);
  bool handleTelemetryAckSummary(uint8_t nodeId, uint8_t seq);
  AckTracker *findOrCreateAckTracker(uint8_t nodeId);
  void resetAckTracker(uint8_t nodeId);
  void recordTelemetrySequence(AckTracker &tracker, uint8_t seq);
  void updateTelemetryReceiptWindow(AckTracker &tracker, uint8_t seq);
  bool sendAckSummary(uint8_t nodeId, uint8_t ackBaseSeq, uint16_t ackMask,
                      const char *reason, uint8_t triggerSeq);
  void maybeSendPeriodicTimeSync();
  BinaryPacket::TimeSyncPayload baseLocalTimeSyncPayload() const;
  void updateJetsonTimeSource(const BinaryPacket::TimeSyncPayload &ts);
  BinaryPacket::TimeSyncPayload currentTimeSyncPayload() const;

  // Reserved-slot (slot 0) TX gating — every base-originated LoRa send
  // funnels through here instead of transmitting immediately, so base
  // traffic can no longer land inside a real node's TX window.
  bool baseTxWindowOpen(uint32_t &slotIndexOut) const;
  void maybeSendInBaseWindow();
  bool sendPendingDirectTimeSync();
  bool sendPendingCommand();
  bool sendPendingAckSummary(uint32_t slotIndex);
  bool enqueuePendingCommand(uint8_t targetNodeId, const uint8_t *payload, uint8_t len);

  bool pushJetsonUartByte(uint8_t b, uint8_t *payloadOut, uint8_t &lenOut);
  void resetJetsonUartRx();
  void maybeLogHealth();
};
