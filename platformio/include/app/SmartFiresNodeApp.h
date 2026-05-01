#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"
#include "radio/PacketHandler.h"
#include "radio/TdmaClock.h"
#include "radio/TdmaRadioService.h"

class SmartFiresNodeApp {
public:
    struct Config {
        uint8_t nodeId;
        uint32_t deviceUidHash;
        bool    enableBattery;
        bool    awakenOnlyMode;

        static SmartFiresNodeApp::Config appCfg(uint8_t nodeId_ = 0,
                                                uint32_t deviceUidHash_ = 0,
                                                bool enableBattery_ = true,
                                                bool awakenOnlyMode_ = true) {
            SmartFiresNodeApp::Config cfg;
            cfg.nodeId        = nodeId_;
            cfg.deviceUidHash = deviceUidHash_;
            cfg.enableBattery = enableBattery_;
            cfg.awakenOnlyMode = awakenOnlyMode_;
            return cfg;
        }
    };

    SmartFiresNodeApp(const Config &cfg, IClock &clock, DutyCycleController &duty,
                      PacketHandler &packetHandler, TdmaRadioService &radio,
                      TdmaClock &tdmaClock,
                      ISensor **sensors, size_t sensorCount,
                      BatteryMonitor *battery);

    bool begin();
    void update();

private:
    static constexpr uint32_t kAwakenIntervalMs = 5000;  // re-send AWAKEN every 5 s until sync

    Config _cfg;

    IClock              &_clock;
    DutyCycleController &_duty;
    PacketHandler       &_packetHandler;
    TdmaRadioService    &_radio;
    TdmaClock           &_tdmaClock;

    ISensor **_sensors;
    size_t    _sensorCount;

    BatteryMonitor *_battery;

    bool     _initialized    = false;
    bool     _syncActive     = false;
    bool     _awakenOnlyNotified = false;
    uint32_t _awakenLastSentMs = 0;
    uint8_t  _awakenSeq        = 0;

    void sendAwakenHandshake();
    SensorSnapshot buildSnapshot() const;
};
