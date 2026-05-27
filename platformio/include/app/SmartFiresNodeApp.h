#pragma once

#include "interfaces/IClock.h"
#include "interfaces/ISensor.h"
#include "power/BatteryMonitor.h"
#include "power/DutyCycleController.h"
#include "radio/PacketHandler.h"
#include "radio/TdmaClock.h"
#include "radio/TdmaRadioService.h"
#include "sensors/Icm20948Sensor.h"

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

    bool begin();
    void update();

private:
    static constexpr uint32_t kAwakenIntervalMs = 5000;  // re-send AWAKEN every 5 s until sync
    static constexpr uint8_t kCalStatusSuccess = 0x00;
    static constexpr uint8_t kCalStatusLowSampleCount = 0x01;
    static constexpr uint8_t kCalStatusError = 0x02;

    enum class CalibrationState : uint8_t {
        Idle,
        Calibrating,
        Uploading,
    };

    struct CalibrationStats {
        uint16_t n = 0;
        float mean[3] = {0.0f, 0.0f, 0.0f};
        float m2_xx = 0.0f;
        float m2_yy = 0.0f;
        float m2_zz = 0.0f;
        float m2_xy = 0.0f;
        float m2_xz = 0.0f;
        float m2_yz = 0.0f;
        float minV[3] = {0.0f, 0.0f, 0.0f};
        float maxV[3] = {0.0f, 0.0f, 0.0f};
        bool minMaxInitialized = false;
    };

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
    CalibrationState _calState = CalibrationState::Idle;
    uint32_t _calStartMs = 0;
    uint32_t _lastCalSampleMs = 0;
    uint32_t _lastCalUploadAttemptMs = 0;
    uint32_t _lastCalProgressLogMs = 0;
    uint8_t _calDurationS = 60;
    uint8_t _cmdSeq = 0;
    uint8_t _calUploadAttemptCount = 0;
    CalibrationStats _calStats = {};
    ISensor *_imuSensor = nullptr;
    bool _calTelemetrySuppressedLogged = false;

    void sendAwakenHandshake();
    SensorSnapshot buildSnapshot() const;
    void handleIncomingCommands();
    void updateCalibrationMode();
    bool maybeCaptureCalibrationSample();
    void resetCalibrationStats();
    void updateCalibrationStats(float mx, float my, float mz);
    bool sendCmdAck(uint8_t cmdType, uint8_t status);
    bool sendCalibrationData(uint8_t status);
    uint32_t calibrationElapsedMs() const;
};
