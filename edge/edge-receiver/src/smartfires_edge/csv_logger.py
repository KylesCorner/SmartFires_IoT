import csv
import os
from pathlib import Path

CSV_COLUMNS = [
    "timestamp",
    "packet_type",
    "node_id",
    "seq",
    "session_time_ms",
    "uptime_ms",
    "sensor_flags",
    "wind_mps",
    "temp_c",
    "humidity_pct",
    "pm1_0_ug_m3",
    "pm2_5_ug_m3",
    "pm4_0_ug_m3",
    "pm10_ug_m3",
    "lat",
    "lon",
    "gps_valid",
    "battery_valid",
    "battery_mv",
    "battery_pct",
    "flags",
    "rssi",
    "uid_hash",
    "heading_true_deg",
    "location_corrected_heading",
    "jetson_wind_mps",
    "jetson_wind_dir_deg",
    "retx_total",
    "fail_total",
    "delta_flags",
    "pkt_flags",
    # Timed duty-cycle window bookkeeping, derived from the PKT_WINDOW_BEGIN /
    # PKT_WINDOW_END frames (see window_state.py). window_id is the reliable one
    # to group on: it is stamped on every row of a window and survives a lost
    # marker, whereas window_first only marks the first row after a BEGIN and
    # window_last only ever appears on the window_end row itself — the true close
    # instant, which no data row can represent because the last bundle's final
    # sample is taken before the window ends. Empty in Continuous mode, which has
    # no windows.
    "window_id",
    "window_first",
    "window_last",
    "planned_sleep_ms",
    "window_sample_count",
    "retx",
    "reset_cause",
    "reset_cause_names",
    "hang_zone",
    "hang_zone_name",
]


class DurableCsvLogger:
    def __init__(self, root: Path, fsync_every_row: bool = False) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        self.fsync_every_row = fsync_every_row
        self._file = None
        self._writer = None

    def _current_path(self) -> Path:
        return self.root / "telemetry.csv"

    def _ensure_open(self) -> None:
        if self._file is not None:
            return
        path = self._current_path()
        exists = path.exists()
        self._file = open(path, "a", newline="", buffering=1)
        self._writer = csv.DictWriter(self._file, fieldnames=CSV_COLUMNS)
        if not exists:
            self._writer.writeheader()
            self._file.flush()
            os.fsync(self._file.fileno())

    def reset(self) -> None:
        """Close the current file (caller creates a new logger for the new session)."""
        self.close()

    def write_row(self, row: dict) -> None:
        self._ensure_open()
        assert self._writer is not None
        assert self._file is not None

        self._writer.writerow(row)
        self._file.flush()

        if self.fsync_every_row:
            os.fsync(self._file.fileno())

    def close(self) -> None:
        if self._file is not None:
            self._file.flush()
            os.fsync(self._file.fileno())
            self._file.close()
            self._file = None
            self._writer = None
