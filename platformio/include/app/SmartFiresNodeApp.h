#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"
#include "radio/PacketHandler.h"
#include "radio/TdmaRadioService.h"

class SmartFiresNodeApp {
public:
    struct Config {
        bool enableBattery;
        static SmartFiresNodeApp::Config appCfg(bool enableBattery_ = true) {
            SmartFiresNodeApp::Config cfg;
            cfg.enableBattery = enableBattery_;
            return cfg;
        }
    };

    SmartFiresNodeApp(const Config &cfg, IClock &clock, DutyCycleController &duty,
                      PacketHandler &packetHandler, TdmaRadioService &radio,
                      ISensor **sensors, size_t sensorCount,
                      BatteryMonitor *battery);

    bool begin();
    void update();

private:
    Config _cfg;

    IClock              &_clock;
    DutyCycleController &_duty;
    PacketHandler       &_packetHandler;
    TdmaRadioService    &_radio;

    ISensor **_sensors;
    size_t    _sensorCount;

    BatteryMonitor *_battery;

    bool _initialized = false;

    SensorSnapshot buildSnapshot() const;
};
