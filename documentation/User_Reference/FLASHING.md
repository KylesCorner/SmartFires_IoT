---
name: flashing
description: PlatformIO flash, monitor, and build commands for every Feather M0 environment, plus upload troubleshooting.
category: reference
status: current
last_verified: 2026-10-15
source_refs:
  - platformio/platformio.ini
related_docs:
  - debug-filter
  - network-test
  - tdma-protocol
---

# SmartFires flashing guide

Run every command from `SmartFires_IoT/platformio`:

```bash
cd ~/Documents/Smart_Fires/SmartFires_IoT/platformio
```

Use `pio`, `platformio`, or `~/.platformio/penv/bin/pio` according to the local installation.

## Active environments

| Environment | Use |
|---|---|
| `feather_m0_lora_base` | Base station connected to the Jetson over native USB |
| `feather_m0_lora_node` | Production Continuous node |
| `feather_m0_lora_node_debug` | Default Timed node with structured/log-to-file monitor filters |
| `feather_m0_lora_node_timed` | Explicit Timed node |
| `feather_m0_lora_node_hybrid` | Hybrid node |
| `feather_m0_lora_sniffer` | Passive LoRa sniffer |
| `native` | Host Unity tests; no Feather |

Ten `feather_m0_power_*` environments isolate MCU run/standby, I2C idle, radio standby/RX, SHT31, IMU, GPS, SPS30, and wind power. List them in `platformio.ini` before choosing one. There is no current dummy-node or sensor-probe target.

All deployment profiles use app-layer ACK summaries. STATUS cadence, sample cadence, TDMA geometry, operational bundle cap, and modem settings come from the shared SF profile. Production differs from debug primarily by duty-cycle mode: Continuous versus Timed.

## Build and upload

```bash
# Build only
pio run -e feather_m0_lora_base
pio run -e feather_m0_lora_node
pio run -e feather_m0_lora_node_debug
pio run -e feather_m0_lora_sniffer

# Build and flash
pio run -e feather_m0_lora_base --target upload
pio run -e feather_m0_lora_node --target upload
pio run -e feather_m0_lora_node_debug --target upload
pio run -e feather_m0_lora_sniffer --target upload
```

Attach a suitable 915 MHz antenna before powering/transmitting with an RFM95. Verify the environment matches the board's physical role before upload.

## Monitor

```bash
pio device monitor -e feather_m0_lora_node_debug
pio device monitor -e feather_m0_lora_base
pio device monitor -e feather_m0_lora_sniffer
```

The base's native USB port carries binary Jetson frames and structured logs, so a plain monitor can display mixed/binary output. The running edge receiver and a serial monitor cannot own the same device simultaneously. See `DEBUG_FILTER.md` for source/level filtering.

## Network-wide changes

`SMARTFIRES_NETWORK_PROFILE` is defined once in the `[network]` section and must be `7`, `9`, `10`, `12`, or `120`. Selector `120` is the two-node SF12 range-test variant: three slots, 15-second STATUS, one-minute sensing, and approximately eight-minute BUNDLE generation in Continuous mode. `NUM_SLOTS` is a matching tripwire: use `3` with selector `120` and `5` with standard profiles. After changing the profile selector:

1. rebuild and reflash the base;
2. rebuild and reflash every node;
3. rebuild and reflash the passive sniffer;
4. start a new recording session before collecting telemetry;
5. verify the dashboard reports the selected SF from the base and no sniffer/override mismatch.

Mixing profiles is unsupported: modem settings and frame periods disagree, so mismatched radios may not hear one another and timing analysis becomes invalid.

## SF12 trial: build-only staging

First edit the shared selector in `platformio.ini`:

```ini
[network]
build_flags =
  -DNUM_SLOTS=3
  -DSMARTFIRES_NETWORK_PROFILE=120
```

Use selector `12` with `NUM_SLOTS=5` to restore the standard SF12 geometry/cadence. Use the Continuous `feather_m0_lora_node` target for the range test; selector `120` retains the standard SF12 timing in Timed builds.

Then build the matching role artifacts without uploading or opening a monitor:

```bash
pio run -e feather_m0_lora_base
pio run -e feather_m0_lora_node
pio run -e feather_m0_lora_node_timed
pio run -e feather_m0_lora_sniffer
```

These commands only compile. Review each startup/profile identity during the later controlled flash trial and confirm it reports `SF12 · 250 kHz · 4/5`, three slots, and a 15-second frame; keep SF12 in STATIC mode for the trial, and do not mix selector `120` artifacts with any other profile build. The five-second slot, 100 ms guards, 15-second STATUS, 60-second sampling, eight-sample bundle, and other range-test values remain provisional until hardware acceptance.

## Upload troubleshooting

For a Feather M0 that does not enter upload mode:

1. Double-tap reset until the bootloader LED pulses.
2. Start the upload within the bootloader window.
3. Confirm a data-capable USB cable and find the port:

   ```bash
   ls /dev/cu.usbmodem*
   ```

4. If needed, specify the resolved port:

   ```bash
   pio run -e feather_m0_lora_base --target upload \
     --upload-port /dev/cu.usbmodem1101
   ```

Replace the example port with the actual device. Disconnect or stop any monitor/service holding it.

## Native tests

```bash
pio test -e native
```

The suite currently has known failures described in `documentation/Possible_Plans/NATIVE_TEST_REPAIR.md`; distinguish those from new build/link failures when reporting results.
