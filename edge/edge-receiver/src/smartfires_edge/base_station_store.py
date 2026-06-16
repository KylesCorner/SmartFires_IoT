import threading
from pathlib import Path
from typing import Optional

from smartfires_edge.state_store import atomic_write_json, read_json


class BaseStationStore:
    def __init__(self, path: Optional[Path] = None) -> None:
        self._path = path or (Path.home() / ".smartfires" / "base_station.json")
        self._lock = threading.Lock()

    def get(self) -> Optional[dict[str, float]]:
        with self._lock:
            raw = read_json(self._path)
        if not raw or "lat" not in raw or "lon" not in raw:
            return None
        return {"lat": float(raw["lat"]), "lon": float(raw["lon"])}

    def set(self, lat: float, lon: float) -> dict[str, float]:
        payload = {"lat": float(lat), "lon": float(lon)}
        with self._lock:
            atomic_write_json(self._path, payload)
        return payload
