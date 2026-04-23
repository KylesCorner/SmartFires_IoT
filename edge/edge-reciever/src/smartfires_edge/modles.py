from dataclasses import dataclass
from typing import Optional


@dataclass(slots=True)
class DecodedPacket:
    timestamp: str
    node_id: int
    seq: int
    session_time_ms: int
    uptime_ms: int
    sensor_flags: int
    wind_mps: float
    temp_c: float
    humidity_pct: float
    pm1_0_ug_m3: float
    pm2_5_ug_m3: float
    pm4_0_ug_m3: float
    pm10_ug_m3: float
    lat: float
    lon: float
    rssi: Optional[int]

    def to_csv_row(self) -> dict:
        return self.__dict__.copy()
