# SmartFires — Flashing Guide

All commands are run from the `platformio/` directory. The PlatformIO CLI is at
`~/.platformio/penv/bin/pio` — it is not on the system PATH by default.

```bash
cd ~/Documents/Smart_Fires/SmartFires_IoT/platformio
```

---

## Devices and their environments

| Device | Board | Environment | Notes |
|---|---|---|---|
| ESP32 sensor node 1 | Arduino Nano ESP32 | `drone` | NODE_ID=1 |
| ESP32 sensor node 2 | Arduino Nano ESP32 | `drone_node2` | NODE_ID=2 |
| Feather M0 — LoRa node 1 | Adafruit Feather M0 RFM95 | `lora_feather` | Paired with ESP32 node 1 |
| Feather M0 — LoRa node 2 | Adafruit Feather M0 RFM95 | `lora_feather_node2` | Paired with ESP32 node 2 |
| Feather M0 — Base station | Adafruit Feather M0 RFM95 | `lora_feather_base` | Connects to Jetson via UART |

---

## Flash commands

### ESP32 — sensor node 1
```bash
~/.platformio/penv/bin/pio run -e drone --target upload
```

### ESP32 — sensor node 2
```bash
~/.platformio/penv/bin/pio run -e drone_node2 --target upload
```

### Feather M0 — LoRa node 1
```bash
~/.platformio/penv/bin/pio run -e lora_feather --target upload
```

### Feather M0 — LoRa node 2
```bash
~/.platformio/penv/bin/pio run -e lora_feather_node2 --target upload
```

### Feather M0 — Base station
```bash
~/.platformio/penv/bin/pio run -e lora_feather_base --target upload
```

---

## Serial monitor commands

Useful for watching debug output after flashing.

### ESP32 node 1
```bash
~/.platformio/penv/bin/pio device monitor -e drone
```

### Feather M0 node 1
```bash
~/.platformio/penv/bin/pio device monitor -e lora_feather
```

### Feather M0 base station
```bash
~/.platformio/penv/bin/pio device monitor -e lora_feather_base
```

---

## Build only (no upload)

To compile without flashing — useful for catching errors:

```bash
~/.platformio/penv/bin/pio run -e drone
~/.platformio/penv/bin/pio run -e lora_feather
~/.platformio/penv/bin/pio run -e lora_feather_base
```

---

## Feather M0 upload troubleshooting

The Feather M0 uses a SAM-BA bootloader that can be timing-sensitive. If the upload
fails partway through or the port is not found, use the manual bootloader trick:

1. **Double-tap the reset button** on the Feather — the red LED will pulse slowly,
   indicating it is in bootloader mode.
2. Run the upload command immediately (within ~10 seconds).

To confirm the Feather is visible to macOS before uploading:
```bash
ls /dev/cu.usbmodem*
```

If nothing appears, the cable may be charge-only. Swap to a data-capable USB cable.

If the port is detected but upload still fails, specify it explicitly:
```bash
~/.platformio/penv/bin/pio run -e lora_feather_base --target upload --upload-port /dev/cu.usbmodem1101
```
(Replace `/dev/cu.usbmodem1101` with whatever `ls /dev/cu.usbmodem*` shows.)

---

## ESP32 upload troubleshooting

If the ESP32 upload fails, hold the **BOOT button** on the board while the upload
starts, then release it once the progress bar begins. Some boards require this to
enter download mode manually.
