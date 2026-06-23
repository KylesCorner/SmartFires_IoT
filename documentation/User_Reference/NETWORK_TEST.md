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
| USB cable (data-capable) | 2 | One per Feather — the base's cable also carries the Jetson link |

> **TIME_SYNC dependency:** The node withholds all sensing until it receives a
> TIME_SYNC packet from the base station. The base station originates LoRa
> TIME_SYNC transmissions (AWAKEN reply + periodic broadcast). If Jetson sync
> is available over the USB link, the base uses Jetson-derived time; otherwise
> it falls back to base-local session time.

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

Connect the base station Feather to the Jetson via USB
(`/dev/smartfires-base`, see UART_JETSON_BRIDGE.md for the udev symlink setup).

```bash
smartfires-edge receive --port /dev/smartfires-base --data-dir /mnt/nvme_drive/data
```

This starts the session clock and sends periodic TIME_SYNC updates to the base
over UART. The base uses these updates as preferred time authority for its own
LoRa TIME_SYNC transmissions.

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

### AWAKEN sequence

1. Node boots and sends AWAKEN.
2. Base receives AWAKEN and sends direct TIME_SYNC to that node.
3. Node applies TIME_SYNC and exits AWAKEN-only behavior.
4. Base continues periodic TIME_SYNC broadcasts.

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

The base sends TIME_SYNC over LoRa (direct reply and periodic maintenance).
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
| TIME_SYNC flows | Base monitor shows `TX TIME_SYNC_LOCAL` and/or `TX TIME_SYNC_PERIODIC` |
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
