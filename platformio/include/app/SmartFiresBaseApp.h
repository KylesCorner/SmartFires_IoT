#pragma once

#include "interfaces/IClock.h"
#include "radio/ITdmaRadioDriver.h"
#include "telemetry/BinaryPacket.h"

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

class SmartFiresBaseApp {
public:
  struct Config {
    uint8_t baseAddr = 0x01;
    uint8_t timeSyncBroadcastAddr = 0xFF;
    uint16_t uartBaud = 115200;

    static Config baseCfg(uint8_t baseAddr_ = 0x01,
                          uint8_t timeSyncBroadcastAddr_ = 0xFF,
                          uint16_t uartBaud_ = 115200) {
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
                    HardwareSerial &debugUart);

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

  Config _cfg;
  IClock &_clock;
  ITdmaRadioDriver &_radio;
  HardwareSerial &_jetsonUart;
  HardwareSerial &_debugUart;

  UartRxState _uartRx;
  bool _initialized = false;

  uint32_t _lastHealthLogMs = 0;
  uint32_t _rxForwardCount = 0;
  uint32_t _cmdForwardCount = 0;
  uint32_t _uartFrameErrorCount = 0;

  void processIncomingLoRa();
  void processIncomingJetsonUart();
  bool handleJetsonCommandPayload(const uint8_t *payload, uint8_t len);

  bool pushJetsonUartByte(uint8_t b, uint8_t *payloadOut, uint8_t &lenOut);
  void resetJetsonUartRx();
  void maybeLogHealth();
};
