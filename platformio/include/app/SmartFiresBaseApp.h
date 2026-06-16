#pragma once

#include "config/BaseConfig.h"
#include "interfaces/IClock.h"
#include "radio/ITdmaRadioDriver.h"
#include "telemetry/BinaryPacket.h"

#include <stddef.h>
#include <stdint.h>

class HardwareSerial;
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
                    HardwareSerial &jetsonUart,
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
  };

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
  HardwareSerial &_jetsonUart;
  Print &_debugUart;

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

  void processIncomingLoRa();
  void processIncomingJetsonUart();
  bool handleJetsonCommandPayload(const uint8_t *payload, uint8_t len);
  bool sendDirectTimeSync(uint8_t radioAddr, uint8_t nodeId, const char *reason,
                          uint8_t triggerSeq = 0);
  NodeAssignment *findOrCreateNodeAssignment(uint32_t uidHash);
  bool handleTelemetryAckSummary(uint8_t nodeId, uint8_t seq);
  AckTracker *findOrCreateAckTracker(uint8_t nodeId);
  void recordTelemetrySequence(AckTracker &tracker, uint8_t seq);
  void updateTelemetryReceiptWindow(AckTracker &tracker, uint8_t seq);
  void maybeSendPendingAckSummaries();
  bool ackSummaryWindowOpen(uint32_t &slotIndexOut) const;
  bool sendAckSummary(uint8_t nodeId, uint8_t ackBaseSeq, uint16_t ackMask,
                      const char *reason, uint8_t triggerSeq);
  void maybeSendPeriodicTimeSync();
  BinaryPacket::TimeSyncPayload baseLocalTimeSyncPayload() const;
  void updateJetsonTimeSource(const BinaryPacket::TimeSyncPayload &ts);
  BinaryPacket::TimeSyncPayload currentTimeSyncPayload() const;

  bool pushJetsonUartByte(uint8_t b, uint8_t *payloadOut, uint8_t &lenOut);
  void resetJetsonUartRx();
  void maybeLogHealth();
};
