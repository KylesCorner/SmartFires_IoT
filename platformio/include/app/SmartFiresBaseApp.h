#pragma once

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
    uint8_t baseAddr = 0x01;
    uint8_t timeSyncBroadcastAddr = 0xFF;
    uint32_t uartBaud = 115200;

    static Config baseCfg(uint8_t baseAddr_ = 0x01,
                          uint8_t timeSyncBroadcastAddr_ = 0xFF,
                          uint32_t uartBaud_ = 115200) {
      Config cfg;
      cfg.baseAddr = baseAddr_;
      cfg.timeSyncBroadcastAddr = timeSyncBroadcastAddr_;
      cfg.uartBaud = uartBaud_;
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

  static constexpr uint32_t kHealthLogPeriodMs = 5000;
  static constexpr uint32_t kPeriodicTimeSyncMs = 5000;

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
  uint32_t _awakenRxCount = 0;
  uint32_t _bundleRxCount = 0;
  uint32_t _statusRxCount = 0;
  uint32_t _fullStateRxCount = 0;
  uint32_t _rawRxCount = 0;
  uint32_t _timeSyncTxCount = 0;
  uint32_t _ackTxCount = 0;
  uint32_t _radioReceiveFailCount = 0;
  uint32_t _lastRxMs = 0;
  uint32_t _lastPeriodicTimeSyncMs = 0;
  uint32_t _sessionId = 0;
  uint8_t _timeSyncSeq = 0;
  bool _hasJetsonTime = false;
  uint32_t _jetsonSessionId = 0;
  uint32_t _jetsonSessionMsAtUpdate = 0;
  uint32_t _localMsAtJetsonUpdate = 0;

  void processIncomingLoRa();
  void processIncomingJetsonUart();
  bool handleJetsonCommandPayload(const uint8_t *payload, uint8_t len);
  bool sendDirectTimeSync(uint8_t nodeId, const char *reason,
                          uint8_t triggerSeq = 0);
  void maybeSendPeriodicTimeSync();
  BinaryPacket::TimeSyncPayload baseLocalTimeSyncPayload() const;
  void updateJetsonTimeSource(const BinaryPacket::TimeSyncPayload &ts);
  BinaryPacket::TimeSyncPayload currentTimeSyncPayload() const;

  bool pushJetsonUartByte(uint8_t b, uint8_t *payloadOut, uint8_t &lenOut);
  void resetJetsonUartRx();
  void maybeLogHealth();
};
