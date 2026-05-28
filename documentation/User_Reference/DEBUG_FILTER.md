# SmartFires Debug Monitor

The SmartFires node debug build prints structured debug logs over USB serial.

Instead of using a plain serial monitor, we use a PlatformIO monitor filter that formats the logs and lets us filter by stream.

Each log line has a source stream such as:

- base
- app
- boot
- i2c
- battery
- gps
- imu
- sht31
- sps30
- wind
- tdma
- radio
- packet
- duty

### Log Levels
| Level | Meaning |
| ----- | ------- |
| T     | Trace   |
| D     | Debug   |
| I     | Info    |
| W     | Warning |
| E     | Error   |

---

## Start the debug monitor

From the PlatformIO project folder:

```bash
cd ~/SmartFires/code/platformio
pio device monitor -e feather_m0_lora_node_debug
```

You can apply filters by the example below showing a tdma filter with a min level of warning.

```bash
SFDBG_SRC=tdma SFDBG_MIN_LEVEL=W pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=radio,tdma,calibration  pio device monitor -e feather_m0_lora_node_debug
```

You can also apply multiple filter streams

```bash
SFDBG_SRC=tdma,i2c,gps SFDBG_MIN_LEVEL=W pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=calib pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=IMU pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=packet SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_node_debug
SFDBG_SRC=packet pio device monitor -e feather_m0_lora_node_debug
```

To watch the exact raw IMU values packed into each STATUS payload, use the
`packet` stream at debug level. Look for `status_imu_payload`, which prints
`mag_xyz` in `uT x 10` and `accel_xyz` in `mg`:

```bash
SFDBG_SRC=packet SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_node_debug
```

For a dedicated IMU troubleshooting stream that is independent of STATUS timing,
also watch `imu_raw_sample` on the `IMU` stream. This emits on each
successful IMU sample and prints raw magnetometer (`uT`), accelerometer (`mg`),
and gyro (`dps`) values:

```bash
SFDBG_SRC=IMU SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_node_debug
```

Base station now uses the same structured logger. To follow communication-layer
traffic on the base station:

```bash
SFDBG_SRC=base,app,calibration SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_base
SFDBG_SRC=base,app,calibration pio device monitor -e feather_m0_lora_base
```

To confirm whether the base is attempting to send app-layer ACK summaries,
watch only the `base` stream at info level. Each transmit attempt logs
`tx_ack_summary_local`:

```bash
SFDBG_SRC=base SFDBG_MIN_LEVEL=I SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_base | tee /tmp/sf-base-ack.log
```

To confirm whether the node is receiving app-layer ACK summaries from the base,
watch only the `radio` stream at info level. A successful receive logs
`ack_summary_received`; a failed inbound decode logs `receive_failed`:

```bash
SFDBG_SRC=radio SFDBG_MIN_LEVEL=I SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_node_debug | tee /tmp/sf-node-ack.log
```

If you want the surrounding packet context as well, raise the same stream to
debug level so you also capture `rx_lora` lines:

```bash
SFDBG_SRC=radio SFDBG_MIN_LEVEL=D SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_node_debug | tee /tmp/sf-node-ack.log
```



Monitor Node Reliability and Packets:
SFDBG_SRC=boot,tdma,radio SFDBG_MIN_LEVEL=D SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_node_debug | tee /tmp/sf-node-debug.log