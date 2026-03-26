# SmartFires LoRa Bring-Up (Two Demo Nodes)

This project now has two PlatformIO environments:

- `lora_tx`: transmits one packet per second
- `lora_rx`: receives packets and prints RSSI

## Hardware assumptions
- Board: Adafruit Feather M0 with RFM95 LoRa (915 MHz)
- Antenna connected before power-up
- Two separate boards connected over USB

## Build and upload
From the `lora` folder:

```bash
# Build both targets
pio run -e lora_tx
pio run -e lora_rx

# Upload TX node (set your serial port)
pio run -e lora_tx -t upload --upload-port /dev/cu.usbmodemXXXX

# Upload RX node (set your serial port)
pio run -e lora_rx -t upload --upload-port /dev/cu.usbmodemYYYY
```

## Serial monitors
Open two terminals:

```bash
# TX monitor
pio device monitor -b 115200 -p /dev/cu.usbmodemXXXX

# RX monitor
pio device monitor -b 115200 -p /dev/cu.usbmodemYYYY
```

Expected output:
- TX prints `TX -> node=...,seq=...,ts=...`
- RX prints `RX <- node=...,seq=...,ts=... | RSSI=...`

## First troubleshooting checks
1. Confirm both boards are Feather M0 LoRa 915 MHz variants.
2. Confirm antenna is attached on both boards.
3. Keep boards close (1-2 meters) for initial test.
4. Confirm only one monitor is attached per serial port.
5. If no receive:
   - reset both boards,
   - verify both use `LORA_FREQ_MHZ = 915.0`,
   - reflash RX first, TX second,
   - swap roles (flash opposite firmware) to rule out hardware faults.

## Next step after link-up
- Expand packet fields to include sensor values and strict sequence counters for packet-delivery-ratio metrics.
