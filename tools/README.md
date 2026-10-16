# Reusable tools

These utilities operate on firmware logs, field telemetry, or deployment
hardware. Generated data and plots should be written outside the repository.

| Tool | Purpose |
|---|---|
| `debug_viewer.py` | Interactive viewer and recorder for structured `@SFDBG` serial logs |
| `lora_sniffer_dashboard.py` | Standalone Panel/Bokeh view of passive-sniffer output |
| `plot_drone_data.py` | Plot synchronized drone and SmartFires data to a chosen HTML output |
| `plot_sht31.py` | Plot SHT31 captures |
| `plot_wind_angle_bench.py` | Plot wind-angle bench captures |
| `log_scope_power.py` | Record Siglent scope measurements to CSV |
| `power_sweep.sh` | Coordinate PlatformIO power targets and scope logging |
| `rsync_from_jetson.sh` | Copy field sessions from a Jetson with `rsync` |
| `lora_tdma_calculator.html` | Offline LoRa airtime, capacity, and energy calculator |
| `udev/99-smartfires.rules` | Stable Linux device names for the base and sniffer |

Run each script with `--help` where supported. Python dependencies vary by
tool and include PySerial, NumPy, pandas, Matplotlib, Plotly, Panel, and Bokeh.
The core edge-service dependencies remain defined separately in
`edge/edge-receiver/pyproject.toml`.
