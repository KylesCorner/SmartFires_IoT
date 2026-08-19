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
#include "radio/TxPowerController.h"
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
  // Max over every command type that can be queued here — CALIBRATE, RESET,
  // and SET_TX_POWER (all 8 bytes today, but written as a max so adding a
  // larger command type can't silently truncate one).
  static constexpr size_t kCalibrateOrResetLoRaSize =
      BinaryPacket::kCmdCalibrateLoRaSize > BinaryPacket::kCmdResetLoRaSize
          ? BinaryPacket::kCmdCalibrateLoRaSize
          : BinaryPacket::kCmdResetLoRaSize;
  static constexpr size_t kPendingCommandPayloadSize =
      kCalibrateOrResetLoRaSize > BinaryPacket::kCmdSetTxPowerLoRaSize
          ? kCalibrateOrResetLoRaSize
          : BinaryPacket::kCmdSetTxPowerLoRaSize;

  struct PendingCommand {
    bool inUse = false;
    uint8_t targetNodeId = 0;
    uint8_t payload[kPendingCommandPayloadSize] = {};
    uint8_t len = 0;
    // Consecutive sendToWait() failures for this entry — see
    // BaseConfig::kMaxPendingCommandSendAttempts.
    uint8_t failedSendAttempts = 0;
  };

  // The Jetson's "New Session" flow (ingest_service.py's reset_event handler)
  // enqueues one CMD_RESET per configured node in a tight loop, so the queue
  // has to hold a command for every node at once or the extras silently fail
  // to enqueue (QUEUE_FULL, visible only in the base debug log) and those
  // nodes never get reset. Sized off kMaxAssignedNodes rather than a literal
  // so it can no longer be outgrown by a NUM_SLOTS bump: the +1 leaves room
  // for one operator-triggered per-node command to coexist with a full
  // network-wide sweep. See documentation/Pending_Plans/RESET_SYSTEM.md.
  static constexpr uint8_t kMaxPendingCommands =
      static_cast<uint8_t>(BaseConfig::kMaxAssignedNodes + 1);

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

    // Bounded retry / circuit-breaker for a node that's gone unreachable.
    // failedSendAttempts counts consecutive sendAckSummary() failures since
    // the last reset; once it reaches kMaxAckSummarySendAttempts,
    // retryHeld suppresses further attempts until genuinely new telemetry
    // arrives from this node (handleTelemetryAckSummary()) or it re-AWAKENs
    // (resetAckTracker()) — both reset both fields to zero/false.
    uint8_t failedSendAttempts = 0;
    bool retryHeld = false;

    // Timed-mode duty-cycle gating, driven by PKT_WINDOW_END/PKT_WINDOW_BEGIN.
    // A node that just sent WINDOW_END is about to enter MCU standby with its
    // radio off: every ACK_SUMMARY aimed at it would be a ~1 s blocking
    // sendToWait (kLinkRetries x kLinkAckTimeoutMs, longer than the base's own
    // 900 ms slot 0) that cannot possibly be heard, starving TIME_SYNC and
    // commands for other nodes. While `asleep`, the tracker keeps `dirty` set
    // and is simply skipped, so the ack is deferred rather than lost — it goes
    // out on the first slot 0 after WINDOW_BEGIN says the node is back, merged
    // with whatever has since been added to the mask.
    //
    // Because the signal now rides its own frame rather than a flag on a
    // retransmittable bundle, the rule is simply "END means asleep, anything
    // else means awake" — no need to tell a fresh window close from a replayed
    // one asserting the opposite. `lastHeard*` remains the fallback for a
    // WINDOW_END that was itself lost, so `asleep` never got set.
    bool asleep = false;
    bool lastHeardValid = false;
    uint32_t lastHeardMs = 0;

    // Set when a RETX-flagged frame or a PKT_WINDOW_BEGIN arrives: both are
    // proof the node never received the last ack (a RETX because it is asking
    // again, a BEGIN because anything sent during its standby was transmitted
    // at a switched-off radio), so this one send must bypass the
    // unchangedFromLastSent suppression that would otherwise silently drop it.
    bool forceResend = false;

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

  // Per-node dynamic TX power decisions. Pure logic, deliberately owning no
  // radio and no clock — this class feeds it observations and transmits the
  // decisions it hands back. See radio/TxPowerController.h.
  TxPowerController _txPower;

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

  // Set by a PKT_WINDOW_BEGIN so the ack deferred across that node's standby
  // goes out in the very next slot 0 rather than waiting on
  // ackSummaryMinIntervalMs. The node is holding unacked entries whose retry
  // gate is on a hold of only a couple of frame periods
  // (TdmaRadioService::kAckRoundTripFrames); spending that budget on a rate
  // limiter meant for steady-state coalescing would let the retransmission the
  // markers exist to prevent fire anyway.
  bool _ackSummaryFlushRequested = false;
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
  bool handleTelemetryAckSummary(uint8_t nodeId, uint8_t seq, uint8_t flags);

  // PKT_WINDOW_BEGIN / PKT_WINDOW_END. Deliberately kept off the sequence path
  // that handleTelemetryAckSummary() runs: markers carry no telemetry seq, so
  // recording one would put a phantom entry in the ack bitmap and the seq20
  // receipt-window loss stats.
  bool handleWindowMarker(uint8_t nodeId, uint8_t pktType,
                          const BinaryPacket::WindowMarkerPayload &marker);
  AckTracker *findOrCreateAckTracker(uint8_t nodeId);
  void resetAckTracker(uint8_t nodeId, const char *reason);
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

  // Encodes and queues one TxPowerController decision, then reports the
  // outcome back to the controller. A failed enqueue must leave the controller
  // un-armed so the decision simply re-arms next interval — see
  // TxPowerController::onCommandFailed().
  bool sendTxPowerDecision(const TxPowerController::Decision &decision);

  bool pushJetsonUartByte(uint8_t b, uint8_t *payloadOut, uint8_t &lenOut);
  void resetJetsonUartRx();
  void maybeLogHealth();
};
