---
name: debug-filter
description: How to use the PlatformIO monitor filter and SFDBG_* env vars to filter structured debug logs by stream and level.
category: reference
status: current
last_verified: 2026-09-04
source_refs:
  - platformio/monitor/filter_smartfires_debug.py
related_docs:
  - flashing
  - packet-reliability
---

# SmartFires debug monitor

The `feather_m0_lora_node_debug` environment enables the `smartfires_debug` and `log2file` monitor filters. Base firmware also emits the same structured `@SFDBG` format, although its environment does not add the filter automatically.

Run from `SmartFires_IoT/platformio`:

```bash
pio device monitor -e feather_m0_lora_node_debug
```

If `pio` is not on `PATH`, use `~/.platformio/penv/bin/pio` or `platformio`.

## Structured record

Firmware emits tab-delimited records similar to:

```text
@SFDBG  v=1  node=2  src=radio  lvl=I  seq=42  t=123456  msg=...
```

The monitor filter parses escaped fields, formats the record, and tracks sequence gaps per node. Non-`@SFDBG` text is shown only when raw display is enabled.

Common source names include `base`, `app`, `boot`, `i2c`, `battery`, `gps`, `imu`, `sht31`, `sps30`, `wind`, `tdma`, `radio`, `packet`, `duty`, and `calib`. Source matching is exact and case-sensitive; use names as they appear in the log (`calib`, not `calibration`).

Levels are `T` trace, `D` debug, `I` info, `W` warning, `E` error, and `O` off. The minimum level includes that severity and everything above it.

## Environment filters

| Variable | Meaning | Default |
|---|---|---|
| `SFDBG_SRC` | Comma-separated source allowlist | all |
| `SFDBG_NODE` | Comma-separated node allowlist | all |
| `SFDBG_MIN_LEVEL` | Minimum severity | `T` |
| `SFDBG_SHOW_RAW` | Show unstructured/raw serial text | `1` |
| `SFDBG_HEADER` | Print filter settings at startup | `1` |
| `SFDBG_LEVEL_NAME` | Expand one-letter level names | `0` |

Examples:

```bash
SFDBG_SRC=tdma,radio SFDBG_MIN_LEVEL=W \
  pio device monitor -e feather_m0_lora_node_debug

SFDBG_SRC=radio SFDBG_MIN_LEVEL=I SFDBG_SHOW_RAW=0 \
  pio device monitor -e feather_m0_lora_node_debug

SFDBG_SRC=base SFDBG_MIN_LEVEL=I SFDBG_SHOW_RAW=0 \
  pio device monitor -e feather_m0_lora_base
```

For several nodes on one forwarded/logged stream:

```bash
SFDBG_NODE=2,3 SFDBG_SRC=radio,packet pio device monitor -e feather_m0_lora_node_debug
```

## Useful searches

- Base ACK transmission: `SFDBG_SRC=base`, then look for `tx_ack_summary_local`.
- Node ACK reception: `SFDBG_SRC=radio`, then look for `ack_summary_received`.
- Join/sync: `SFDBG_SRC=boot,tdma,radio`.
- Command handling and reset: `SFDBG_SRC=calib,app,base`.
- TX-power decisions: `SFDBG_SRC=base,radio`.

STATUS contains fused heading/accuracy, not raw magnetometer, accelerometer, or gyro vectors. Use the IMU driver's own debug messages for sensor-level diagnosis; do not expect raw axes in the wire payload.

## Saving output

`node_debug` already enables `log2file`. For an explicit shell capture:

```bash
SFDBG_SRC=boot,tdma,radio SFDBG_MIN_LEVEL=D SFDBG_SHOW_RAW=0 \
  pio device monitor -e feather_m0_lora_node_debug | tee /tmp/sf-node-debug.log
```

Opening a serial monitor takes ownership of the serial device and can conflict with the Jetson receiver or another monitor. Stop the competing process first.
