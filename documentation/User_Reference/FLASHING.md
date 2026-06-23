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
| Feather M0 — LoRa node | Adafruit Feather M0 RFM95 | `feather_m0_lora_node` | Production sensor node firmware |
| Feather M0 — LoRa node (debug) | Adafruit Feather M0 RFM95 | `feather_m0_lora_node_debug` | Same as above, lower sample rate, structured debug logging (see DEBUG_FILTER.md) |
| Feather M0 — Base station | Adafruit Feather M0 RFM95 | `feather_m0_lora_base` | Connects to Jetson via UART |
| Feather M0 — Sensor probe | Adafruit Feather M0 RFM95 | `feather_m0_sensor_probe` | No LoRa/TDMA/app layer; for sensor bring-up and power measurement |
| Feather M0 — LoRa sniffer | Adafruit Feather M0 RFM95 | `feather_m0_lora_sniffer` | Passive listener, prints packet metadata, does not participate in TDMA |
| Native unit tests | n/a (runs on dev machine) | `native` | `pio test -e native` |

---

## Flash commands

### Feather M0 — LoRa node
```bash
~/.platformio/penv/bin/pio run -e feather_m0_lora_node --target upload
```

### Feather M0 — LoRa node (debug build)
```bash
~/.platformio/penv/bin/pio run -e feather_m0_lora_node_debug --target upload
```

### Feather M0 — Base station
```bash
~/.platformio/penv/bin/pio run -e feather_m0_lora_base --target upload
```

### Feather M0 — Sensor probe
```bash
~/.platformio/penv/bin/pio run -e feather_m0_sensor_probe --target upload
```

### Feather M0 — LoRa sniffer
```bash
~/.platformio/penv/bin/pio run -e feather_m0_lora_sniffer --target upload
```

---

## Serial monitor commands

Useful for watching debug output after flashing.

### Feather M0 node (debug build)
```bash
~/.platformio/penv/bin/pio device monitor -e feather_m0_lora_node_debug
```

### Feather M0 base station
```bash
~/.platformio/penv/bin/pio device monitor -e feather_m0_lora_base
```

See DEBUG_FILTER.md for `SFDBG_*` environment variables that filter the
structured debug log stream.

---

## Build only (no upload)

To compile without flashing — useful for catching errors:

```bash
~/.platformio/penv/bin/pio run -e feather_m0_lora_node
~/.platformio/penv/bin/pio run -e feather_m0_lora_base
~/.platformio/penv/bin/pio run -e feather_m0_sensor_probe
~/.platformio/penv/bin/pio run -e feather_m0_lora_sniffer
~/.platformio/penv/bin/pio test -e native
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
~/.platformio/penv/bin/pio run -e feather_m0_lora_base --target upload --upload-port /dev/cu.usbmodem1101
```
(Replace `/dev/cu.usbmodem1101` with whatever `ls /dev/cu.usbmodem*` shows.)
