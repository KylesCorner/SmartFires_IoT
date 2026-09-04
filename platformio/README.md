# SmartFires firmware

This PlatformIO project builds every Feather M0 firmware role and the native Unity tests. The current protocol and timing references live in [`../documentation/Current_Architecture/`](../documentation/Current_Architecture/); `platformio.ini` is authoritative for active build flags.

## Network targets

| Environment | Purpose | Important build settings |
|---|---|---|
| `feather_m0_lora_base` | LoRa base and native-USB bridge to the Jetson | `LORA_BASE=1`, `NUM_SLOTS=5` |
| `feather_m0_lora_node` | Production sensor node | SensorTriggered duty cycle, app-layer ACK summary, 15 s STATUS |
| `feather_m0_lora_node_debug` | Default debug node with log filters | Timed duty cycle, app-layer ACK summary, 15 s STATUS |
| `feather_m0_lora_node_timed` | Explicit Timed node profile | Timed duty cycle, app-layer ACK summary, 15 s STATUS |
| `feather_m0_lora_node_hybrid` | Hybrid duty-cycle profile | Hybrid duty cycle, app-layer ACK summary, 15 s STATUS |
| `feather_m0_lora_sniffer` | Passive LoRa sniffer emitting NDJSON over USB | Sniffer-only entrypoint |
| `native` | Host-side Unity tests | No Arduino hardware |

The project also has isolated `feather_m0_power_*` environments for MCU, I2C, radio, and individual-sensor power measurements. See `platformio.ini` for the complete list. Removed historical targets such as `feather_m0_lora_node_dummy` and `feather_m0_sensor_probe` must not be used.

## Network model

`NUM_SLOTS=5` is shared by the base and all node targets. Slot 0 is the base; the remaining four slots are assignable to nodes. Nodes start unassigned, broadcast `AWAKEN` with a SAMD21 UID hash, then adopt the node ID returned by the base in direct `TIME_SYNC`. The base is node ID/address 1, so assigned sensor IDs begin at 2.

Changing `NUM_SLOTS` requires reflashing every network Feather and updating the edge receiver's `DEFAULT_NUM_SLOTS`. Mismatched frame geometry causes collisions and can prevent later nodes from receiving an assignment.

The base talks to the Jetson through native USB CDC (`Serial`) at 115200 baud. A node uses `Serial1` for its SPS30 sensor.

## Runtime notes

- Active node targets use `SMARTFIRES_TDMA_RELIABILITY_MODE=1`: telemetry is link-layer fire-and-forget and recovered through base-generated `ACK_SUMMARY` packets.
- `AWAKEN` is an out-of-band join packet; direct and periodic `TIME_SYNC` establish/refresh session time.
- Timed windows emit `WINDOW_BEGIN` and `WINDOW_END`. Two complete 15-sample bundles are produced by the normal 30-second/1-second Timed active window.
- The base forwards received packets to the Jetson, decodes STATUS for its TX-power controller, assigns IDs, sends sync, and owns acknowledgement summaries.
- `CMD_RESET` performs real node or base reset behavior. `CMD_CALIBRATE` remains log-and-ACK by design because the ICM-20948 DMP self-calibrates.

## Commands

Run from this directory. See [`commands.txt`](commands.txt) for the full short list.

```bash
pio run -e feather_m0_lora_base
pio run -e feather_m0_lora_node
pio run -e feather_m0_lora_node_debug

pio run -e feather_m0_lora_base --target upload
pio run -e feather_m0_lora_node --target upload

pio device monitor -e feather_m0_lora_node_debug
pio test -e native
```

The native suite currently has known failures documented in [`../documentation/Pending_Plans/NATIVE_TEST_REPAIR.md`](../documentation/Pending_Plans/NATIVE_TEST_REPAIR.md). Treat a red run as known debt until that plan is completed, but record any new compiler or linker failures separately.
