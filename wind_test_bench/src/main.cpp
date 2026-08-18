#include <Arduino.h>
#include <Servo.h>
#include <math.h>

// -----------------------------
// Pin configuration
// -----------------------------
static constexpr uint8_t SERVO_PIN = 9;

static constexpr uint8_t WIND_PIN = A0;
static constexpr uint8_t TEMP_PIN = A1;

static constexpr uint8_t FAN_PWM_PIN = 3;
static constexpr uint8_t FAN_TACH_PIN = 2;

static constexpr uint8_t FAN_POWER_RELAY_PIN = 7;

// Most sensor-kit relay modules are active LOW.
// If your relay behavior is backwards, change this to true.
static constexpr bool RELAY_ACTIVE_HIGH = true;

// -----------------------------
// ADC / sensor configuration
// -----------------------------
static constexpr float ADC_REF_VOLTS = 5.0f;
static constexpr float ADC_MAX_COUNTS = 1023.0f;

static constexpr float ZERO_WIND_ADJUST_VOLTS = 0.0f;

// -----------------------------
// Servo configuration
// -----------------------------
static constexpr int SERVO_MIN_DEG = 0;
static constexpr int SERVO_MAX_DEG = 180;
static constexpr int SERVO_START_DEG = 0;

static constexpr uint32_t SERVO_STEP_PERIOD_MS = 1000;
static constexpr int SERVO_STEP_DEG = 5;

// -----------------------------
// Fan configuration
// -----------------------------
static constexpr uint8_t FAN_PWM_TOP = 79;

static constexpr uint8_t FAN_CONTROL_PASS_PWM_PERCENT = 0;
static constexpr uint8_t FAN_RUN_PWM_PERCENT_DEFAULT = 100;

static constexpr float FAN_TACH_PULSES_PER_REV = 2.0f;

static constexpr uint32_t FAN_SPINUP_DELAY_MS = 5000;

// -----------------------------
// Test sequence configuration
// -----------------------------
static constexpr uint32_t WIND_SENSOR_WARMUP_MS = 15000;

// -----------------------------
// Sampling configuration
// -----------------------------
static uint32_t samplePeriodMs = 250;

static constexpr uint8_t NUM_SAMPLES = 16;
static constexpr uint16_t SAMPLE_DELAY_US = 100;

// -----------------------------
// State
// -----------------------------
Servo servo;

static int servoDeg = SERVO_START_DEG;
static int servoDirection = 1;
static bool sweepEnabled = true;

static uint32_t servoPassIndex = 0;
static bool fanEnabledForCurrentPass = false;

static uint8_t fanRunPwmPercent = FAN_RUN_PWM_PERCENT_DEFAULT;
static uint8_t currentFanPwmPercent = FAN_CONTROL_PASS_PWM_PERCENT;

static uint32_t lastServoStepMs = 0;
static uint32_t lastSampleMs = 0;
static uint32_t servoHoldUntilMs = 0;

static volatile uint32_t fanTachPulseCount = 0;

static uint32_t lastTachComputeMs = 0;
static uint32_t lastTachPulseCount = 0;
static float fanTachHz = 0.0f;
static float fanRpm = 0.0f;

// -----------------------------
// Fan tach ISR
// -----------------------------
static void fanTachIsr()
{
    ++fanTachPulseCount;
}

// -----------------------------
// Utility
// -----------------------------
static float adcToVolts(float adc)
{
    return adc * ADC_REF_VOLTS / ADC_MAX_COUNTS;
}

static uint16_t readAnalogAverage(uint8_t pin)
{
    uint32_t sum = 0;

    for (uint8_t i = 0; i < NUM_SAMPLES; ++i)
    {
        sum += analogRead(pin);
        delayMicroseconds(SAMPLE_DELAY_US);
    }

    return static_cast<uint16_t>(sum / NUM_SAMPLES);
}

// -----------------------------
// Relay / fan power control
// -----------------------------
static void fanPowerEnable(bool enabled)
{
    const bool outputHigh = RELAY_ACTIVE_HIGH ? enabled : !enabled;
    digitalWrite(FAN_POWER_RELAY_PIN, outputHigh ? HIGH : LOW);
}

// -----------------------------
// Fan PWM control
// -----------------------------
static void fanPwmDetachTimerAndDrive(bool transistorOn)
{
    TCCR2A &= ~(_BV(COM2B1) | _BV(COM2B0));

    pinMode(FAN_PWM_PIN, OUTPUT);
    digitalWrite(FAN_PWM_PIN, transistorOn ? HIGH : LOW);
}

static void fanPwmEnableTimer(uint8_t transistorOnPercent)
{
    if (transistorOnPercent > 100)
    {
        transistorOnPercent = 100;
    }

    pinMode(FAN_PWM_PIN, OUTPUT);

    uint16_t highCounts =
        ((static_cast<uint16_t>(FAN_PWM_TOP) + 1u) *
         static_cast<uint16_t>(transistorOnPercent)) /
        100u;

    if (highCounts < 1)
    {
        highCounts = 1;
    }

    if (highCounts > FAN_PWM_TOP)
    {
        highCounts = FAN_PWM_TOP;
    }

    const uint8_t compareValue = static_cast<uint8_t>(highCounts - 1u);

    cli();

    TCCR2A = 0;
    TCCR2B = 0;
    TCNT2 = 0;

    OCR2A = FAN_PWM_TOP;
    OCR2B = compareValue;

    TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20);
    TCCR2B = _BV(WGM22) | _BV(CS21);

    sei();
}

static void setFanPwmPercent(uint8_t fanDutyPercent)
{
    if (fanDutyPercent > 100)
    {
        fanDutyPercent = 100;
    }

    currentFanPwmPercent = fanDutyPercent;

    if (fanDutyPercent == 0)
    {
        fanPwmDetachTimerAndDrive(true);
        return;
    }

    if (fanDutyPercent >= 100)
    {
        fanPwmDetachTimerAndDrive(false);
        return;
    }

    const uint8_t transistorOnPercent = 100 - fanDutyPercent;
    fanPwmEnableTimer(transistorOnPercent);
}

static void updateFanForCurrentPass()
{
    fanEnabledForCurrentPass = servoPassIndex > 0;

    if (fanEnabledForCurrentPass)
    {
        fanPowerEnable(true);
        setFanPwmPercent(fanRunPwmPercent);
    }
    else
    {
        setFanPwmPercent(FAN_CONTROL_PASS_PWM_PERCENT);
        fanPowerEnable(false);
    }
}

static void updateFanTachEstimate(uint32_t nowMs)
{
    uint32_t pulseCountSnapshot = 0;

    noInterrupts();
    pulseCountSnapshot = fanTachPulseCount;
    interrupts();

    if (lastTachComputeMs == 0)
    {
        lastTachComputeMs = nowMs;
        lastTachPulseCount = pulseCountSnapshot;
        fanTachHz = 0.0f;
        fanRpm = 0.0f;
        return;
    }

    const uint32_t elapsedMs = nowMs - lastTachComputeMs;

    if (elapsedMs == 0)
    {
        return;
    }

    const uint32_t deltaPulses = pulseCountSnapshot - lastTachPulseCount;

    fanTachHz =
        static_cast<float>(deltaPulses) * 1000.0f /
        static_cast<float>(elapsedMs);

    fanRpm =
        fanTachHz * 60.0f /
        FAN_TACH_PULSES_PER_REV;

    lastTachComputeMs = nowMs;
    lastTachPulseCount = pulseCountSnapshot;
}

// -----------------------------
// Wind calculation
// -----------------------------
static float estimateZeroWindVolts(uint16_t rawTmp)
{
    const float tmp = static_cast<float>(rawTmp);

    const float zeroWindAdUnits =
        (-0.0006f * tmp * tmp) +
        (1.0727f * tmp) +
        47.172f;

    float zeroWindVolts = adcToVolts(zeroWindAdUnits);
    zeroWindVolts -= ZERO_WIND_ADJUST_VOLTS;

    return zeroWindVolts;
}

static float estimateWindMph(float windVolts, float zeroWindVolts)
{
    const float delta = windVolts - zeroWindVolts;

    if (delta <= 0.0f)
    {
        return 0.0f;
    }

    return powf(delta / 0.2300f, 2.7265f);
}

// -----------------------------
// Servo / sweep control
// -----------------------------
static void setServoAngle(int angle)
{
    if (angle < SERVO_MIN_DEG)
    {
        angle = SERVO_MIN_DEG;
    }

    if (angle > SERVO_MAX_DEG)
    {
        angle = SERVO_MAX_DEG;
    }

    servoDeg = angle;
    servo.write(servoDeg);
}

static bool isSpinupHoldActive(uint32_t now)
{
    return servoHoldUntilMs != 0 && now < servoHoldUntilMs;
}

static void resetSweepSequence()
{
    servoPassIndex = 0;
    servoDirection = 1;
    sweepEnabled = true;
    servoHoldUntilMs = 0;

    setServoAngle(SERVO_START_DEG);
    updateFanForCurrentPass();

    const uint32_t now = millis();
    lastServoStepMs = now;
    lastSampleMs = now;
    lastTachComputeMs = now;
}

static void updateServoSweep()
{
    if (!sweepEnabled)
    {
        return;
    }

    const uint32_t now = millis();

    if (isSpinupHoldActive(now))
    {
        return;
    }

    if (servoHoldUntilMs != 0 && now >= servoHoldUntilMs)
    {
        servoHoldUntilMs = 0;
        lastServoStepMs = now;
        lastSampleMs = now;
        lastTachComputeMs = now;
        return;
    }

    if (now - lastServoStepMs < SERVO_STEP_PERIOD_MS)
    {
        return;
    }

    lastServoStepMs = now;

    int nextServoDeg = servoDeg + servoDirection * SERVO_STEP_DEG;
    bool passFinished = false;

    if (nextServoDeg >= SERVO_MAX_DEG)
    {
        nextServoDeg = SERVO_MAX_DEG;
        servoDirection = -1;
        passFinished = true;
    }
    else if (nextServoDeg <= SERVO_MIN_DEG)
    {
        nextServoDeg = SERVO_MIN_DEG;
        servoDirection = 1;
        passFinished = true;
    }

    setServoAngle(nextServoDeg);

    if (passFinished)
    {
        const uint32_t oldPassIndex = servoPassIndex;

        ++servoPassIndex;
        updateFanForCurrentPass();

        if (oldPassIndex == 0 && servoPassIndex == 1)
        {
            servoHoldUntilMs = millis() + FAN_SPINUP_DELAY_MS;

            const uint32_t holdStart = millis();
            lastSampleMs = holdStart;
            lastTachComputeMs = holdStart;

            noInterrupts();
            lastTachPulseCount = fanTachPulseCount;
            interrupts();
        }
    }
}

// -----------------------------
// CSV output
// -----------------------------
static void printCsvHeader()
{
    Serial.println(
        "t_ms,"
        "servo_pass,"
        "fan_enabled,"
        "servo_deg,"
        "raw_wind,"
        "wind_v,"
        "raw_tmp,"
        "tmp_v,"
        "zero_v,"
        "wind_mph,"
        "wind_mps,"
        "fan_pwm_percent,"
        "fan_tach_hz,"
        "fan_rpm,"
        "fan_tach_pulses_total"
    );
}

static void sampleAndPrint()
{
    const uint32_t now = millis();

    if (isSpinupHoldActive(now))
    {
        return;
    }

    if (now - lastSampleMs < samplePeriodMs)
    {
        return;
    }

    lastSampleMs = now;

    updateFanTachEstimate(now);

    const uint16_t rawWind = readAnalogAverage(WIND_PIN);
    const uint16_t rawTmp = readAnalogAverage(TEMP_PIN);

    const float windVolts = adcToVolts(rawWind);
    const float tmpVolts = adcToVolts(rawTmp);

    const float zeroWindVolts = estimateZeroWindVolts(rawTmp);
    const float windMph = estimateWindMph(windVolts, zeroWindVolts);
    const float windMps = windMph * 0.44704f;

    uint32_t tachPulseSnapshot = 0;

    noInterrupts();
    tachPulseSnapshot = fanTachPulseCount;
    interrupts();

    Serial.print(now);
    Serial.print(',');

    Serial.print(servoPassIndex);
    Serial.print(',');

    Serial.print(fanEnabledForCurrentPass ? 1 : 0);
    Serial.print(',');

    Serial.print(servoDeg);
    Serial.print(',');

    Serial.print(rawWind);
    Serial.print(',');

    Serial.print(windVolts, 3);
    Serial.print(',');

    Serial.print(rawTmp);
    Serial.print(',');

    Serial.print(tmpVolts, 3);
    Serial.print(',');

    Serial.print(zeroWindVolts, 3);
    Serial.print(',');

    Serial.print(windMph, 3);
    Serial.print(',');

    Serial.print(windMps, 3);
    Serial.print(',');

    Serial.print(currentFanPwmPercent);
    Serial.print(',');

    Serial.print(fanTachHz, 2);
    Serial.print(',');

    Serial.print(fanRpm, 1);
    Serial.print(',');

    Serial.println(tachPulseSnapshot);
}

// -----------------------------
// Serial commands
// -----------------------------
static void handleCommand(String cmd)
{
    cmd.trim();
    cmd.toLowerCase();

    if (cmd.length() == 0)
    {
        return;
    }

    if (cmd == "reset")
    {
        resetSweepSequence();
        return;
    }

    if (cmd == "sweep on")
    {
        sweepEnabled = true;
        return;
    }

    if (cmd == "sweep off")
    {
        sweepEnabled = false;
        return;
    }

    if (cmd.startsWith("angle "))
    {
        const int angle = cmd.substring(6).toInt();
        sweepEnabled = false;
        setServoAngle(angle);
        return;
    }

    if (cmd.startsWith("rate "))
    {
        const int requestedRate = cmd.substring(5).toInt();

        if (requestedRate >= 20)
        {
            samplePeriodMs = static_cast<uint32_t>(requestedRate);
        }

        return;
    }

    if (cmd.startsWith("fan "))
    {
        const int requestedFanPwm = cmd.substring(4).toInt();

        if (requestedFanPwm >= 0 && requestedFanPwm <= 100)
        {
            fanRunPwmPercent = static_cast<uint8_t>(requestedFanPwm);
            updateFanForCurrentPass();
        }

        return;
    }

    if (cmd == "relay on")
    {
        fanPowerEnable(true);
        return;
    }

    if (cmd == "relay off")
    {
        fanPowerEnable(false);
        return;
    }

    if (cmd == "header")
    {
        printCsvHeader();
        return;
    }
}

static void pollSerial()
{
    static String line;

    while (Serial.available() > 0)
    {
        const char c = static_cast<char>(Serial.read());

        if (c == '\n' || c == '\r')
        {
            if (line.length() > 0)
            {
                handleCommand(line);
                line = "";
            }
        }
        else
        {
            line += c;

            if (line.length() > 80)
            {
                line = "";
            }
        }
    }
}

// -----------------------------
// Arduino setup / loop
// -----------------------------
void setup()
{
    Serial.begin(115200);

    const bool relayOffOutputHigh = RELAY_ACTIVE_HIGH ? false : true;
    digitalWrite(FAN_POWER_RELAY_PIN, relayOffOutputHigh ? HIGH : LOW);
    pinMode(FAN_POWER_RELAY_PIN, OUTPUT);
    fanPowerEnable(false);

    setFanPwmPercent(FAN_CONTROL_PASS_PWM_PERCENT);

    pinMode(FAN_TACH_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), fanTachIsr, FALLING);

    servo.attach(SERVO_PIN);
    setServoAngle(SERVO_START_DEG);

    delay(WIND_SENSOR_WARMUP_MS);

    servoPassIndex = 0;
    servoDirection = 1;
    fanRunPwmPercent = FAN_RUN_PWM_PERCENT_DEFAULT;

    updateFanForCurrentPass();

    const uint32_t now = millis();
    lastServoStepMs = now;
    lastSampleMs = now;
    lastTachComputeMs = now;

    printCsvHeader();
}

void loop()
{
    pollSerial();
    updateServoSweep();
    sampleAndPrint();
}