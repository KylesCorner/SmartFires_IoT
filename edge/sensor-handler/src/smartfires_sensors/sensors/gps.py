import adafruit_gps


class PA1010D:
    ADDRESS = 0x10

    def __init__(self, i2c):
        self._gps = adafruit_gps.GPS_GtopI2C(
            i2c,
            debug=False,
        )

        # RMC + GGA
        self._gps.send_command(
            b"PMTK314,0,1,0,1,0,0,0,0,0,"
            b"0,0,0,0,0,0,0,0,0"
        )

        # 1 Hz GPS update
        self._gps.send_command(b"PMTK220,1000")

    def update(self):
        """Service incoming NMEA data."""
        self._gps.update()

    def sample(self):
        ts = self._gps.timestamp_utc

        timestamp = None

        if ts is not None:
            timestamp = (
                f"{ts.tm_year:04d}-"
                f"{ts.tm_mon:02d}-"
                f"{ts.tm_mday:02d}T"
                f"{ts.tm_hour:02d}:"
                f"{ts.tm_min:02d}:"
                f"{ts.tm_sec:02d}Z"
            )

        return {
            "sensor": "gps",
            "timestamp_utc": timestamp,
            "has_fix": self._gps.has_fix,
            "latitude": self._gps.latitude,
            "longitude": self._gps.longitude,
            "altitude_m": self._gps.altitude_m,
            "satellites": self._gps.satellites,
        }
