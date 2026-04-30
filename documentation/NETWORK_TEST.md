# SmartFires — Network Integration Test Guide

End-to-end pipeline verification using synthetic sensor data. Tests the full
LoRa → base → Jetson path without any real sensors wired to the node Feather.

---

## Overview

The `feather_m0_lora_node_dummy` firmware environment substitutes two stub
sensor objects for all real hardware:

| Stub | Replaces | What it provides |
|---|---|---|
| `DummySht31Driver` | `AdafruitSht31Driver` | Fixed 25 °C / 50 %RH so `Sht31Sensor` initialises cleanly (satisfies `DutyCycleController`'s trigger-sensor requirement) |
| `DummySensor` | GPS, IMU, SPS30, wind sensors | Triangle-wave oscillating readings over a 60-sample period — gives `PacketHandler` real deltas to compress |

Everything else is identical to a production node: same TDMA slot logic, same
binary packet encoding, same AWAKEN handshake, same STATUS + BUNDLE sequence.

---

## Hardware Required

| Component | Quantity | Notes |
|---|---|---|
| Adafruit Feather M0 RFM95 | 1 | Node — runs `feather_m0_lora_node_dummy` |
| Adafruit Feather M0 RFM95 | 1 | Base station — runs `feather_m0_lora` |
| Jetson Orin Nano | 1 | Runs `smartfires-edge receive`; sends TIME_SYNC to base |
| USB cable (data-capable) | 2 | One per Feather |
| UART cable | 1 | Feather base → Jetson (Serial1 / `/dev/ttyTHS1`) |

> **TIME_SYNC dependency:** The node withholds all sensing until it receives a
> TIME_SYNC broadcast from the base station.  The base station only broadcasts
> TIME_SYNC when it receives one from the Jetson over UART.  The Jetson must be
> running `smartfires-edge receive` for the node to progress past the AWAKEN
> phase and begin transmitting BUNDLE and STATUS packets.

---

## Step-by-Step Procedure

All `pio` commands run from `platformio/`. Use `~/.platformio/penv/bin/pio` if
`pio` is not on your PATH.

### 1 — Flash the base station

```bash
pio run -e feather_m0_lora --target upload
```

### 2 — Flash the dummy node

```bash
pio run -e feather_m0_lora_node_dummy --target upload
```

### 3 — Start the Jetson edge receiver

Connect the base station Feather to the Jetson via Serial1 (`/dev/ttyTHS1`).

```bash
smartfires-edge receive --port /dev/ttyTHS1 --data-dir /mnt/nvme_drive/data
```

This starts the session clock and begins sending TIME_SYNC frames to the base
every 10 minutes (first one is sent immediately on startup).

### 4 — Open serial monitors

Open two terminals (one per Feather):

```bash
# Base station
pio device monitor -e feather_m0_lora

# Dummy node (separate terminal)
pio device monitor -e feather_m0_lora_node_dummy
```

---

## Expected Behaviour

### Phase 1 — AWAKEN (before TIME_SYNC)

**Node serial:**
```
[DUMMY] SmartFires node starting in synthetic data mode
[DUMMY] NODE_ID=1  NUM_SLOTS=2
[DUMMY] app ready — transmitting synthetic data
```
The node broadcasts a 5-byte AWAKEN packet every 5 seconds until sync is
received. No sensor activity yet.

**Base serial:**
```
[BaseApp] Ready
[BaseApp] rx_fwd=1 cmd_fwd=0 uart_err=0   ← AWAKEN relayed to Jetson
```

### Phase 2 — TIME_SYNC received

The Jetson sends a TIME_SYNC UART frame; the base relays it over LoRa.
The node's `TdmaClock::applySync()` fires, `hasSync()` becomes true, and
sensing begins immediately.

**Node serial** (after sync): no explicit log, but BUNDLE and STATUS packets
start appearing in the base's health log within seconds.

### Phase 3 — Active telemetry

**Base serial** (health log every 5 s):
```
[BaseApp] rx_fwd=4 cmd_fwd=1 uart_err=0
[BaseApp] rx_fwd=9 cmd_fwd=1 uart_err=0
```
`rx_fwd` increments with each BUNDLE or STATUS packet received and forwarded to
the Jetson.

**Jetson CSV** (`/mnt/nvme_drive/data/*.csv`): rows appear for each expanded
bundle sample.  Expect slowly-oscillating values for wind, PM, lat/lon (static),
and 25 °C / 50 %RH from the dummy SHT31.

---

## Success Criteria

| Check | Indicator |
|---|---|
| LoRa link alive | Base `rx_fwd` increments (even in AWAKEN phase) |
| TIME_SYNC flows | Base `cmd_fwd` = 1 after Jetson starts |
| Node synced and sensing | `rx_fwd` increases by ≥1 every ~15 s (one BUNDLE per duty cycle) |
| STATUS packets relayed | Jetson CSV contains non-zero `lat_deg` / `lon_deg` (37.7456 / −119.5936) |
| Delta encoding works | BUNDLE packets arrive at ~194 bytes max; Jetson CSV shows smoothly varying values |
| No UART framing errors | `uart_err` stays at 0 on base serial |

---

## Multi-Node Dummy Test

To test TDMA slot isolation with two dummy nodes:

1. Edit `platformio.ini` — add a second dummy env:

```ini
[env:feather_m0_lora_node_dummy_2]
; ... identical to feather_m0_lora_node_dummy except:
build_flags =
  ...
  -DNODE_ID=2
  -DNUM_SLOTS=2
```

2. Flash the second Feather with the new env.
3. Both nodes will transmit in alternating TDMA slots (slot 0 and slot 1).
4. Confirm `rx_fwd` on the base increments faster (two nodes transmitting).

> **Reminder:** `NUM_SLOTS` must match across all node Feathers.  If you change
> it, reflash every node.

---

## Troubleshooting

**Node stuck in AWAKEN / `rx_fwd` not incrementing past 1:**
- Confirm the Jetson is running `smartfires-edge receive`.
- Confirm the UART cable between base and Jetson is wired correctly (TX→RX, RX→TX, GND→GND).
- Check `uart_err` on the base — a non-zero count means framing errors on the Jetson UART side.

**Base serial shows nothing / radio begin failed:**
- Double-tap reset, reflash.
- Check USB cable is data-capable.
- Confirm antenna is attached to both Feathers (the RFM95 can be damaged by TX without antenna).

**Jetson CSV not updating:**
- Check `smartfires-edge receive` output for parse errors.
- The first BUNDLE only appears after `PacketHandler` accumulates 15 samples (~7.5 s at 500 ms sample rate + duty cycle warmup of ~10 s).  Allow ~30 s from sync before expecting CSV rows.
