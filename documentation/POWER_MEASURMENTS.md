---
name: power-measurements
description: Hardware setup and per-environment procedure for isolated Feather M0 power measurements.
category: reference
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/platformio.ini
related_docs:
  - duty-cycling
  - tunable-parameters
---

# SmartFires Power Test Environments

This document describes the PlatformIO environments used to measure SmartFires power draw on the Adafruit Feather M0 LoRa sensor node.

The power-test firmware is compiled with:

```ini
-DPOWER_TEST=1
-DPOWER_TEST_MODE=<mode>
```

Each PlatformIO environment selects one test mode. The purpose is to isolate one subsystem at a time, measure its input power with the oscilloscope/logger, and compare that result against the baseline MCU run current.

## Measurement Hardware Setup

The power tests use a USB low-side shunt measurement.

### USB shunt cable

The shunt resistor is inserted into the USB ground wire:

```text
USB source GND / black wire
        |
      [ shunt resistor ]
        |
Feather USB GND / device-side black wire
```

The USB 5 V, D+, and D- wires remain connected normally:

```text
Red   +5 V  ---------------- unchanged
White D-    ---------------- unchanged
Green D+    ---------------- unchanged
Black GND   ---- shunt ----- Feather GND
```

### Oscilloscope setup

Use one scope channel across the low-side shunt:

```text
CH1 ground clip -> USB source-side ground
CH1 probe tip   -> Feather/device-side ground
```

The scope math/measurement setup converts this shunt voltage into current and then power.

The scope measurement used by the logger should be:

```text
CUST3: MATH, MEAN
```

The logger should query `CUST3` and treat it as mean power in watts.

Example sanity check:

```text
PAVA CUST3:MATH,MEAN,0.08
```

means:

```text
0.08 W = 80 mW
```

### Important setup rules

During these tests:

```text
LiPo unplugged
Only one USB cable connected
No external programmer/debugger connected
No external powered sensor supply unless that branch is intentionally under test
No alternate ground path around the shunt
```

Any alternate ground path can bypass the shunt and make the measured current too low.

## Baseline Calculation

Most test results are interpreted as a difference from the MCU baseline.

```text
subsystem_power = test_power - mcu_run_power
```

For example:

```text
SHT31 system cost = feather_m0_power_sht31 - feather_m0_power_mcu_run
Radio RX cost     = feather_m0_power_radio_rx - feather_m0_power_mcu_run
Sleep savings     = feather_m0_power_mcu_run - feather_m0_power_mcu_standby
```

The absolute value is still useful, but the difference is what shows the cost of each subsystem.

---

# Environment Reference

## `power_test_common`

This is not flashed directly. It is a shared PlatformIO config block used by all power-test environments.

### Purpose

Provides the common source filter, libraries, and build flags for power measurement firmware.

### Shared flags

```ini
-DPOWER_TEST=1
-DICM_20948_USE_DMP
-DPOWER_TEST_USE_SERIAL=1
-DPOWER_TEST_SAMPLE_PERIOD_MS=1000
-DPOWER_TEST_PRE_SLEEP_DELAY_MS=5000
```

### Notes

`POWER_TEST_USE_SERIAL=1` keeps USB serial logging enabled before and during most tests. This is useful for confirming which test firmware is running, but USB serial itself contributes to board power. For ultra-low-power sleep tests, the firmware detaches USB before entering standby.

---

## `feather_m0_power_mcu_run`

### Build flag

```ini
-DPOWER_TEST_MODE=1
```

### What it tests

This is the main baseline test.

It measures the Feather M0 LoRa board with:

```text
SAMD21 MCU running
USB serial active
Sensors disabled
Application disabled
TDMA/radio networking disabled
RFM95 LoRa radio placed into sleep mode by raw SPI
```

### Hardware setup

Use the standard USB shunt setup.

Recommended physical state:

```text
No LiPo
No sensors connected, or sensors physically present but not externally powered
No external sensor supply
No second USB/debug connection
```

### Expected use

This is the value most other tests are compared against.

Example interpretation:

```text
MCU run baseline = 80 mW
SHT31 test       = 83 mW
SHT31 cost       = 3 mW
```

### Notes

This is not “bare SAMD21 only.” It includes board-level overhead:

```text
Feather regulator losses
USB interface overhead
Power LED
SAMD21 running
RFM95 physically powered but commanded to sleep
```

---

## `feather_m0_power_mcu_standby`

### Build flag

```ini
-DPOWER_TEST_MODE=2
```

### What it tests

This measures the board after the SAMD21 enters deep standby.

The firmware:

```text
Boots normally
Puts the RFM95 LoRa radio into sleep
Prints a startup message
Waits POWER_TEST_PRE_SLEEP_DELAY_MS
Detaches USB
Stops SPI
Disables ADC
Sets SLEEPDEEP
Enters WFI standby forever
```

### Hardware setup

Use the standard USB shunt setup.

Recommended physical state:

```text
No LiPo
No sensors connected or externally powered
Only the shunt USB cable connected
```

### Expected behavior

The power trace should show two regions:

```text
Startup region: normal running current
Sleep region: lower steady current after standby
```

Only use the post-sleep steady region for the result.

### Notes

Because this test is still powered through USB, the measured sleep floor may be dominated by Feather board overhead rather than the SAMD21 itself. For true battery sleep current, repeat later from the LiPo/BAT path with USB disconnected.

---

## `feather_m0_power_i2c_idle`

### Build flag

```ini
-DPOWER_TEST_MODE=3
```

### What it tests

This measures the additional cost of initializing the I2C bus without actively sampling a sensor.

The firmware:

```text
Runs the MCU
Puts RFM95 into sleep
Initializes Wire/I2C
Optionally scans the I2C bus
Does not start a specific sensor
```

### Hardware setup

Use the standard USB shunt setup.

Connect the I2C devices exactly as they are connected in the real node if you want to measure bus pullup/device idle effects.

Typical I2C wiring:

```text
Feather 3V  -> sensor VCC
Feather GND -> sensor GND
Feather SDA -> sensor SDA
Feather SCL -> sensor SCL
```

### Expected use

Compare against `feather_m0_power_mcu_run`.

```text
I2C idle cost = i2c_idle_power - mcu_run_power
```

### Notes

This test is useful for catching hidden idle draw from I2C pullups or connected-but-unsampled I2C devices.

---

## `feather_m0_power_radio_standby`

### Build flag

```ini
-DPOWER_TEST_MODE=4
```

### What it tests

This measures the Feather board with the RFM95/SX127x radio placed in LoRa standby mode instead of sleep.

The firmware:

```text
Boots the MCU
Resets the RFM95
Uses raw SPI to set LoRa standby mode
Keeps app/TDMA disabled
Does not transmit
Does not receive continuously
```

### Hardware setup

Use the standard USB shunt setup.

No sensors are needed.

### Expected use

Compare against `feather_m0_power_mcu_run`.

```text
radio standby cost = radio_standby_power - mcu_run_power
```

### Notes

Standby should draw more than radio sleep, but much less than RX or TX.

---

## `feather_m0_power_radio_rx`

### Build flag

```ini
-DPOWER_TEST_MODE=5
```

### What it tests

This intentionally forces the RFM95/SX127x radio into continuous receive mode.

The firmware:

```text
Boots the MCU
Resets the RFM95
Uses raw SPI to set LoRa RX continuous mode
Does not run TDMA
Does not transmit
```

### Hardware setup

Use the standard USB shunt setup.

No sensors are needed.

### Expected use

This is the proof test that the measurement system can see the radio current.

Compare against `feather_m0_power_mcu_run`.

```text
radio RX cost = radio_rx_power - mcu_run_power
```

### Expected result

This should produce a clear increase over the MCU baseline. If the power does not increase meaningfully, either the radio mode command is not taking effect or the measurement setup is not measuring the full board current.

---

## `feather_m0_power_sht31`

### Build flag

```ini
-DPOWER_TEST_MODE=10
```

### What it tests

This measures the system-level power cost of the SHT31 temperature/humidity sensor.

The firmware:

```text
Runs the MCU
Puts RFM95 into sleep
Initializes I2C
Begins the SHT31 driver
Wakes the SHT31 sensor abstraction
Samples periodically
Prints telemetry if serial logging is enabled
```

### Hardware setup

Connect only the SHT31 if possible.

Typical wiring:

```text
Feather 3V  -> SHT31 VIN/VCC
Feather GND -> SHT31 GND
Feather SDA -> SHT31 SDA
Feather SCL -> SHT31 SCL
```

### Expected use

Compare against the MCU baseline or I2C idle baseline:

```text
SHT31 total system cost = sht31_power - mcu_run_power
SHT31 sensor-only-ish cost = sht31_power - i2c_idle_power
```

### Notes

The second subtraction is usually better if `i2c_idle` was measured with the same I2C bus wiring.

---

## `feather_m0_power_imu`

### Build flag

```ini
-DPOWER_TEST_MODE=11
```

### What it tests

This measures the system-level power cost of the SparkFun ICM-20948 IMU.

The firmware:

```text
Runs the MCU
Puts RFM95 into sleep
Initializes I2C
Begins the ICM-20948 driver
Wakes the IMU sensor abstraction
Samples periodically
Prints telemetry if serial logging is enabled
```

### Hardware setup

Connect only the IMU if possible.

Typical wiring:

```text
Feather 3V  -> IMU VCC
Feather GND -> IMU GND
Feather SDA -> IMU SDA
Feather SCL -> IMU SCL
```

### Expected use

Compare against the MCU baseline or I2C idle baseline:

```text
IMU total system cost = imu_power - mcu_run_power
IMU sensor-only-ish cost = imu_power - i2c_idle_power
```

### Notes

If the IMU is configured for DMP mode, its current may differ from a simple low-rate raw-read mode. The `ICM_20948_USE_DMP` build flag is enabled in the shared power-test configuration.

---

## `feather_m0_power_gps`

### Build flag

```ini
-DPOWER_TEST_MODE=12
```

### What it tests

This measures the system-level power cost of the PA1010D GPS.

The firmware:

```text
Runs the MCU
Puts RFM95 into sleep
Begins the GPS driver
Uses FullPowerContinuous mode for the GPS test
Samples/services GPS periodically
Prints telemetry if serial logging is enabled
```

### Hardware setup

Wire the GPS exactly as used in the node.

Typical UART-style setup:

```text
Feather 3V or VIN -> GPS VIN, depending on module requirement
Feather GND       -> GPS GND
Feather TX        -> GPS RX
Feather RX        -> GPS TX
```

### Expected use

Compare against the MCU baseline:

```text
GPS system cost = gps_power - mcu_run_power
```

### Notes

GPS power depends heavily on mode:

```text
Full power continuous
Periodic standby
Periodic backup
AlwaysLocate modes
```

This environment is intended to measure a worst-case or active GPS profile. If later you want GPS sleep/backup mode numbers, add separate GPS-specific power-test environments.

---

## `feather_m0_power_sps30`

### Build flag

```ini
-DPOWER_TEST_MODE=13
```

### What it tests

This measures the system-level power cost of the SPS30 particulate sensor and its UART interface.

The firmware:

```text
Runs the MCU
Puts RFM95 into sleep
Starts Serial1 at 115200
Begins the SPS30 driver
Wakes the SPS30 sensor abstraction
Samples/services periodically
Prints telemetry if serial logging is enabled
```

### Hardware setup

The SPS30 should be connected the same way it is used in the SmartFires node.

Typical setup:

```text
5 V supply/TPS output -> SPS30 VCC
Common GND            -> SPS30 GND
Feather Serial1 TX    -> SPS30 RX
Feather Serial1 RX    -> SPS30 TX
```

If the SPS30 is powered through a TPS boost module, there are two useful measurements:

```text
1. Whole system USB input power
2. TPS input branch power
```

The whole-system test tells you the node-level cost. The TPS input branch test tells you the true converter-plus-sensor cost.

### Expected use

Compare against the MCU baseline:

```text
SPS30 system cost = sps30_power - mcu_run_power
```

### Notes

The SPS30 has startup, fan, active measurement, and sleep behavior. Log long enough to capture the steady active region after startup.

---

## `feather_m0_power_wind`

### Build flag

```ini
-DPOWER_TEST_MODE=14
```

### What it tests

This measures the system-level power cost of the Wind Sensor Rev C branch and its TPS enable pin.

The firmware:

```text
Runs the MCU
Puts RFM95 into sleep
Initializes analog input support
Controls the wind sensor TPS enable pin
Wakes the wind sensor abstraction
Samples RV and TMP periodically
Prints telemetry if serial logging is enabled
```

### Hardware setup

Use the SmartFires wind sensor wiring.

Expected firmware pins:

```text
A1 -> wind RV analog output
A2 -> wind TMP analog output
A3 -> wind TPS enable
```

Typical power setup:

```text
TPS output 5 V -> wind sensor VCC
Common GND     -> wind sensor GND
Wind RV        -> voltage divider -> A1
Wind TMP       -> voltage divider -> A2
Feather A3     -> TPS enable
```

### Expected use

Compare against the MCU baseline:

```text
wind branch system cost = wind_power - mcu_run_power
```

### Notes

For the wind sensor branch, the TPS boost module can be a major part of the power cost. If possible, also measure the TPS input current directly with the shunt placed in series with the TPS input.

---

# Recommended Test Order

Run the environments in this order:

```text
1. feather_m0_power_mcu_run
2. feather_m0_power_mcu_standby
3. feather_m0_power_i2c_idle
4. feather_m0_power_radio_standby
5. feather_m0_power_radio_rx
6. feather_m0_power_sht31
7. feather_m0_power_imu
8. feather_m0_power_gps
9. feather_m0_power_sps30
10. feather_m0_power_wind
```

This order establishes the board baseline first, then verifies MCU sleep, then characterizes shared buses/radio states, then measures each sensor branch.

# Suggested CSV Naming

Use one CSV per environment:

```text
00_mcu_run.csv
01_mcu_standby.csv
02_i2c_idle.csv
03_radio_standby.csv
04_radio_rx.csv
10_sht31.csv
11_imu.csv
12_gps.csv
13_sps30.csv
14_wind.csv
```

Example logger command:

```bash
python log_scope_power.py \
  --measurement CUST3 \
  --duration 600 \
  --interval 1 \
  --csv-path /run/media/kyle/YOUR_DRIVE/smartfires_power/00_mcu_run.csv
```

# Result Table Template

Use this table in the report:

| Test             | Environment                      | Mean Power | Mean Current Equivalent | Delta From MCU Run | Notes                         |
| ---------------- | -------------------------------- | ---------: | ----------------------: | -----------------: | ----------------------------- |
| MCU run baseline | `feather_m0_power_mcu_run`       |            |                         |                  0 | Radio sleep, sensors off      |
| MCU standby      | `feather_m0_power_mcu_standby`   |            |                         |                    | USB detached, standby forever |
| I2C idle         | `feather_m0_power_i2c_idle`      |            |                         |                    | I2C initialized only          |
| Radio standby    | `feather_m0_power_radio_standby` |            |                         |                    | RFM95 standby                 |
| Radio RX         | `feather_m0_power_radio_rx`      |            |                         |                    | RFM95 RX continuous           |
| SHT31            | `feather_m0_power_sht31`         |            |                         |                    | Temperature/humidity sensor   |
| IMU              | `feather_m0_power_imu`           |            |                         |                    | ICM-20948                     |
| GPS              | `feather_m0_power_gps`           |            |                         |                    | PA1010D full-power continuous |
| SPS30            | `feather_m0_power_sps30`         |            |                         |                    | PM sensor + UART              |
| Wind sensor      | `feather_m0_power_wind`          |            |                         |                    | Wind Rev C + TPS enable       |

# Interpretation Notes

The most important number is usually not the absolute power of a test. It is the delta from the baseline:

```text
delta_power = test_power - mcu_run_power
```

This isolates what each subsystem costs relative to the board simply running.

For battery-life estimation, convert mean power to approximate LiPo current:

```text
battery_current_A ≈ mean_power_W / battery_voltage_V
```

For a rough estimate, use:

```text
battery_current_mA ≈ mean_power_W / 3.7 V * 1000
```

Then:

```text
runtime_hours ≈ usable_capacity_mAh / average_current_mA
```

Use a usable-capacity factor for LiPo batteries, usually around 70–85% of rated capacity.
