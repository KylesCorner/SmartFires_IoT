import csv
import os
from datetime import datetime, timezone
from pathlib import Path

CSV_COLUMNS = [
    "timestamp",
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
    "rssi",
    "jetson_wind_mps",
    "jetson_wind_dir_deg",
]


class DurableCsvLogger:
    def __init__(self, root: Path, fsync_every_row: bool = False) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        self.fsync_every_row = fsync_every_row
        self._file = None
        self._writer = None
        self._current_date = None

    def _current_path(self) -> Path:
        today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        return self.root / f"telemetry-{today}.csv"

    def _ensure_open(self) -> None:
        path = self._current_path()
        current_date = path.stem

        if self._file is not None and self._current_date == current_date:
            return

        if self._file is not None:
            self._file.flush()
            os.fsync(self._file.fileno())
            self._file.close()

        exists = path.exists()
        self._file = open(path, "a", newline="", buffering=1)
        self._writer = csv.DictWriter(self._file, fieldnames=CSV_COLUMNS)
        self._current_date = current_date

        if not exists:
            self._writer.writeheader()
            self._file.flush()
            os.fsync(self._file.fileno())

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
