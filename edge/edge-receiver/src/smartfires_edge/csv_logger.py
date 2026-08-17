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
    "window_first",
    "window_last",
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
