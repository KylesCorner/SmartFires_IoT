# Environmental run — 2026-09-03 (no burn)

Source: `analysis and scripts/data/2026-09-03_231038/` (copied verbatim, checksums verified).

## The run

- **Session id:** `0x28BC871A`, started 2026-09-03T23:10:38Z
- **Span:** 2026-09-03 23:10 → 2026-09-04 13:49 UTC = **14.6 h continuous, no gaps > 5 min**
  (local MDT = UTC−6, so 17:10 Sep 3 → 07:49 Sep 4)
- **Nodes:** 2 and 4 carry the run. Node 3 appears for 28 min at the start
  (150 rows, RSSI −115 dBm) and then drops out — exclude it.
- **Site:** Missoula MT, 46.8065, −114.0473

## Contents

| File | What it is |
|---|---|
| `telemetry.csv` | 47,737 rows total; 42,150 are `packet_type == telemetry` |
| `status.jsonl` | per-node status frames (battery lives here, not in telemetry.csv) |
| `packet_loss_state.json` | end-of-run link stats |
| `session.json` | node registry, port, session id |
| `ingest_log.log` | edge ingest log |

`base_debug.log` (362 MB raw serial dump) was left in the source folder.

## Row counts and ranges

| | node 2 | node 4 |
|---|---|---|
| telemetry rows | 20,955 | 21,045 |
| span | 09-03 23:10 → 09-04 13:49 (14.64 h) | 09-03 23:11 → 09-04 13:48 (14.62 h) |
| median cadence | 1.0 s | 1.0 s |
| temp_c | 8.4 – 22.4 (mean 11.8) | 9.0 – 18.3 (mean 11.7) |
| humidity_pct | 48.0 – 93.6 (mean 82.1) | 63.5 – 94.0 (mean 85.1) |
| pm2_5_ug_m3 | 0.0 – 7.7 (mean 1.6) | 0.4 – 7.7 (mean 1.5) |
| pm10_ug_m3 | 0.0 – 13.5 (mean 1.8) | 0.4 – 11.4 (mean 1.7) |
| rssi | −112 – −51 (mean −75.9) | −103 – −38 (mean −69.2) |

`sensor_flags == 19` on every telemetry row (same sensor set throughout).

## Known issues — read before plotting

1. **`telemetry.csv` is NUL-padded.** ~2 KB of trailing NUL bytes and a
   truncated final row, from an rsync of a file that was still open. Parse with
   `pd.read_csv(..., on_bad_lines='skip')` or strip the tail first. Left as-is
   here so the file matches the original byte for byte.
2. **`wind_mps` is not m/s.** Uncalibrated Modern Device Rev C hot-wire
   (`zeroWindAdjustmentVolts` never tuned — see
   `SmartFires_IoT/platformio/include/sensors/WindSensorRevC.h`). Node 3 reports
   up to 209 "m/s". Do not plot as wind speed.
3. **No Jetson ground truth for this run.** `data/ground_truth/sensor-data/`
   only starts 2026-09-05. If the poster needs a BME688/anemometer/GPS/IMU
   reference alongside the nodes, use the 2026-09-05_204746 session instead
   (6.4 h, 3 nodes, 6.29 h of overlap with `20260905T205525Z`).
4. **`jetson_wind_mps`, `jetson_wind_dir_deg`, `heading_true_deg` are empty**
   in every September session.
5. **Battery columns are empty on telemetry rows** — battery_mv / battery_pct
   only appear on `status` rows.
