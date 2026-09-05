---
name: flashing
description: PlatformIO flash, monitor, and build commands for every Feather M0 environment, plus upload troubleshooting.
category: reference
status: current
last_verified: 2026-09-04
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
| `feather_m0_lora_node` | Production SensorTriggered node |
| `feather_m0_lora_node_debug` | Default Timed node with structured/log-to-file monitor filters |
| `feather_m0_lora_node_timed` | Explicit Timed node |
| `feather_m0_lora_node_hybrid` | Hybrid node |
| `feather_m0_lora_sniffer` | Passive LoRa sniffer |
| `native` | Host Unity tests; no Feather |

Ten `feather_m0_power_*` environments isolate MCU run/standby, I2C idle, radio standby/RX, SHT31, IMU, GPS, SPS30, and wind power. List them in `platformio.ini` before choosing one. There is no current dummy-node or sensor-probe target.

All current node targets use app-layer ACK summaries and a 15-second STATUS interval. Production differs from debug primarily by its duty-cycle mode; debug STATUS is not faster than production.

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

`NUM_SLOTS` is shared through the `[network]` section. After changing it:

1. rebuild and reflash the base;
2. rebuild and reflash every node;
3. update edge `DEFAULT_NUM_SLOTS` or the dashboard `--num-slots` argument;
4. recheck retry timing and bandwidth constraints.

Mixing different slot counts is unsafe because frame periods disagree and base assignment capacity changes.

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
