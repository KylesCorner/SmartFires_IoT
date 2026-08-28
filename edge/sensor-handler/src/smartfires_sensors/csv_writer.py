import csv
import time
from datetime import datetime, timezone
from pathlib import Path


class SensorCsvWriter:
    """Writes SmartFires sensor samples to per-sensor CSV files."""

    SCHEMAS = {
        "imu": [
            "host_epoch_ms",
            "accel_x_mps2",
            "accel_y_mps2",
            "accel_z_mps2",
            "gyro_x_dps",
            "gyro_y_dps",
            "gyro_z_dps",
            "mag_x_ut",
            "mag_y_ut",
            "mag_z_ut",
        ],

        "gps": [
            "host_epoch_ms",
            "timestamp_utc",
            "has_fix",
            "latitude",
            "longitude",
            "altitude_m",
            "satellites",
        ],

        "bme688": [
            "host_epoch_ms",
            "temperature_c",
            "humidity_percent",
            "pressure_hpa",
            "gas_resistance_ohm",
        ],

        "anemometer": [
            "host_epoch_ms",
            "speed_mps",
            "direction_deg",
        ],
    }

    def __init__(
        self,
        output_dir: str | Path,
        flush_interval_s: float = 1.0,
    ):
        self.output_dir = Path(output_dir)
        self.flush_interval_s = flush_interval_s

        # Give each acquisition run its own directory.
        session_name = datetime.now(
            timezone.utc
        ).strftime("%Y%m%dT%H%M%SZ")

        self.session_dir = self.output_dir / session_name
        self.session_dir.mkdir(
            parents=True,
            exist_ok=True,
        )

        self._files = {}
        self._writers = {}

        for sensor, fields in self.SCHEMAS.items():
            path = self.session_dir / f"{sensor}.csv"

            file = path.open(
                "w",
                newline="",
                encoding="utf-8",
            )

            writer = csv.DictWriter(
                file,
                fieldnames=fields,
                extrasaction="ignore",
            )

            writer.writeheader()

            self._files[sensor] = file
            self._writers[sensor] = writer

        self._last_flush = time.monotonic()

    def write(self, sensor: str, sample: dict):
        if sensor not in self._writers:
            raise ValueError(
                f"Unknown sensor type: {sensor}"
            )

        row = dict(sample)

        # Timestamp when the Jetson received/read the measurement.
        row.setdefault(
            "host_epoch_ms",
            time.time_ns() // 1_000_000,
        )

        self._writers[sensor].writerow(row)

        now = time.monotonic()

        if (
            now - self._last_flush
            >= self.flush_interval_s
        ):
            self.flush()
            self._last_flush = now

    def flush(self):
        for file in self._files.values():
            file.flush()

    def close(self):
        self.flush()

        for file in self._files.values():
            file.close()

        self._files.clear()
        self._writers.clear()

    def __enter__(self):
        return self

    def __exit__(
        self,
        exc_type,
        exc_value,
        traceback,
    ):
        self.close()
