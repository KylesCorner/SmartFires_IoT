// ---
// description: Top-level node application class — drives the AWAKEN/TIME_SYNC handshake, duty cycle, snapshot building, telemetry enqueueing, and CMD_CALIBRATE/CMD_RESET handling.
// role: implementation
// ---
#pragma once

#include "config/NetworkConfig.h"
#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"
#include "radio/PacketHandler.h"
#include "radio/TdmaClock.h"
#include "radio/TdmaRadioService.h"
#include "platform/IMcuSleep.h"

class SmartFiresNodeApp {
public:
    struct Config {
        uint8_t nodeId;
        uint32_t deviceUidHash;
        bool    enableBattery;
        bool    awakenOnlyMode;
        bool    enableTelemetryTx;

        static SmartFiresNodeApp::Config appCfg(uint8_t nodeId_ = 0,
                                                uint32_t deviceUidHash_ = 0,
                                                bool enableBattery_ = true,
                                                bool awakenOnlyMode_ = true,
                                                bool enableTelemetryTx_ = true) {
            SmartFiresNodeApp::Config cfg;
            cfg.nodeId        = nodeId_;
            cfg.deviceUidHash = deviceUidHash_;
            cfg.enableBattery = enableBattery_;
            cfg.awakenOnlyMode = awakenOnlyMode_;
            cfg.enableTelemetryTx = enableTelemetryTx_;
            return cfg;
        }
    };

    SmartFiresNodeApp(const Config &cfg, IClock &clock, DutyCycleController &duty,
                      PacketHandler &packetHandler, TdmaRadioService &radio,
                      TdmaClock &tdmaClock,
                      ISensor **sensors, size_t sensorCount,
                      BatteryMonitor *battery);
                      SmartFiresNodeApp(
    const Config &cfg,
    IClock &clock,
    DutyCycleController &duty,
    PacketHandler &packetHandler,
    TdmaRadioService &radio,
    TdmaClock &tdmaClock,
    IMcuSleep &mcuSleep,
    ISensor **sensors,
    size_t sensorCount,
    BatteryMonitor *battery);

    bool begin();
    void update();

private:
    static constexpr uint32_t kAwakenIntervalMs = NetworkConfig::kAwakenIntervalMs;
    static constexpr uint8_t kCalStatusSuccess = 0x00;

    Config _cfg;

    IClock              &_clock;
    DutyCycleController &_duty;
    PacketHandler       &_packetHandler;
    TdmaRadioService    &_radio;
    TdmaClock           &_tdmaClock;
    IMcuSleep           &_mcuSleep;

    ISensor **_sensors;
    size_t    _sensorCount;

    BatteryMonitor *_battery;

    bool     _initialized    = false;
    bool     _syncActive     = false;
    bool     _awakenOnlyNotified = false;
    uint32_t _awakenLastSentMs = 0;
    uint8_t  _awakenSeq        = 0;
    uint8_t _cmdSeq = 0;

    bool _forceRadioAwake = false;
    bool _mcuSleptThisCycle = false;

    // Phase 1 instrumentation (rtc-subsecond-sleep): offset of the
    // pre-sleep session clock relative to the local sleep-compensated
    // clock (sessionNowMs() - millis()). Projecting it forward at
    // resync time isolates RTC sleep error from the awake gap spent
    // waiting for the fresh TIME_SYNC. Compared against the new sync
    // to measure real-world error vs guardMs.
    uint32_t _predictedSessionOffsetMs = 0;
    bool _predictedValid = false;

    void sendAwakenHandshake();
    SensorSnapshot buildSnapshot() const;
    void handleIncomingCommands();
    bool sendCmdAck(uint8_t cmdType, uint8_t status);
    bool maybeEnterTimedMcuSleep();
};
