"""Disk-backed OSM tile cache with online fetch and background pre-fetch.

Tiles are stored as {cache_dir}/{z}/{x}/{y}.png, mirroring the standard
XYZ tile URL structure so they can also be served directly as static files.

Pre-fetch policy: on each new plotted location, download zoom 12-17 tiles
within a 2-mile (~3.22 km) radius in a background thread pool at ≤10 req/s
(within OpenStreetMap's acceptable-use policy).
"""

from __future__ import annotations

import logging
import math
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

log = logging.getLogger(__name__)

_OSM_TILE_URL = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
_USER_AGENT = "SmartFiresDashboard/1.0 (wildfire-IoT; offline-capable; contact: smartfires-dev)"
_PREFETCH_ZOOMS = (12, 13, 14, 15, 16, 17)
_RADIUS_M = 3_218.7          # 2 statute miles in metres
_PREFETCH_DELAY_S = 0.1      # 10 req/s — well within OSM policy
_FETCH_TIMEOUT_S = 8.0
_ONLINE_TTL_S = 60.0         # re-check connectivity at most every 60 s


# ---------------------------------------------------------------------------
# Tile coordinate math
# ---------------------------------------------------------------------------

def _tile_xy(lat: float, lon: float, zoom: int) -> tuple[int, int]:
    lat_r = math.radians(lat)
    n = 1 << zoom
    x = int((lon + 180.0) / 360.0 * n)
    y = int(
        (1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi)
        / 2.0
        * n
    )
    return x, y


def _tiles_in_radius(
    lat: float, lon: float, zoom: int, radius_m: float = _RADIUS_M
) -> set[tuple[int, int]]:
    """Return all tile (x, y) pairs within *radius_m* metres of (lat, lon)."""
    lat_r = math.radians(lat)
    cos_lat = math.cos(lat_r)
    dlat = radius_m / 111_320.0
    dlon = radius_m / (111_320.0 * cos_lat) if cos_lat > 1e-6 else 180.0

    n = 1 << zoom
    x0, y0 = _tile_xy(lat + dlat, lon - dlon, zoom)
    x1, y1 = _tile_xy(lat - dlat, lon + dlon, zoom)
    x_min, x_max = sorted((x0, x1))
    y_min, y_max = sorted((y0, y1))

    return {
        (x, y)
        for x in range(max(0, x_min), min(n - 1, x_max) + 1)
        for y in range(max(0, y_min), min(n - 1, y_max) + 1)
    }


# ---------------------------------------------------------------------------
# Blocking network helpers (run in thread-pool executor)
# ---------------------------------------------------------------------------

def _fetch_tile_bytes(z: int, x: int, y: int) -> bytes | None:
    url = _OSM_TILE_URL.format(z=z, x=x, y=y)
    req = urllib.request.Request(url, headers={"User-Agent": _USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=_FETCH_TIMEOUT_S) as resp:
            if resp.status == 200:
                return resp.read()
    except Exception:
        pass
    return None


def _check_online() -> bool:
    req = urllib.request.Request(
        "https://tile.openstreetmap.org/",
        method="HEAD",
        headers={"User-Agent": _USER_AGENT},
    )
    try:
        urllib.request.urlopen(req, timeout=3.0)
        return True
    except Exception:
        return False


# ---------------------------------------------------------------------------
# TileCache
# ---------------------------------------------------------------------------

class TileCache:
    """
    Disk-backed XYZ tile cache.

    - ``get_tile(z, x, y)`` — async; returns cached bytes immediately or
      fetches from OSM and caches before returning.
    - ``prefetch_location(lat, lon)`` — non-blocking; queues a background
      download of all uncached tiles in a 2-mile radius for zoom 12-17.
    - ``is_online()`` — synchronous connectivity check with 60 s TTL cache;
      safe to call from any thread.
    """

    def __init__(self, cache_dir: Path) -> None:
        self._dir = cache_dir
        self._dir.mkdir(parents=True, exist_ok=True)

        # Separate executors so prefetch never blocks real-time tile requests.
        self._fetch_pool = ThreadPoolExecutor(max_workers=4, thread_name_prefix="tile-fetch")
        self._prefetch_pool = ThreadPoolExecutor(max_workers=2, thread_name_prefix="tile-prefetch")

        # Connectivity state — refreshed at most every _ONLINE_TTL_S seconds.
        self._online: bool = False
        self._online_ts: float = 0.0
        self._online_lock = threading.Lock()

        # Deduplicate prefetch requests by (lat, lon) rounded to ~100 m.
        self._prefetch_seen: set[tuple[float, float]] = set()
        self._prefetch_lock = threading.Lock()

    # ------------------------------------------------------------------
    # Internal path helpers
    # ------------------------------------------------------------------

    def _path(self, z: int, x: int, y: int) -> Path:
        return self._dir / str(z) / str(x) / f"{y}.png"

    def _read_cache(self, z: int, x: int, y: int) -> bytes | None:
        p = self._path(z, x, y)
        return p.read_bytes() if p.exists() else None

    def _write_cache(self, z: int, x: int, y: int, data: bytes) -> None:
        p = self._path(z, x, y)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    # ------------------------------------------------------------------
    # Connectivity
    # ------------------------------------------------------------------

    def is_online(self) -> bool:
        """Return cached connectivity state; re-checks at most every 60 s."""
        with self._online_lock:
            if time.monotonic() - self._online_ts >= _ONLINE_TTL_S:
                self._online = _check_online()
                self._online_ts = time.monotonic()
                log.debug("connectivity check: %s", "online" if self._online else "offline")
            return self._online

    # ------------------------------------------------------------------
    # Tile serving
    # ------------------------------------------------------------------

    async def get_tile(self, z: int, x: int, y: int) -> bytes | None:
        """Return tile bytes. Serves from disk cache; falls back to live OSM fetch."""
        import asyncio

        data = self._read_cache(z, x, y)
        if data:
            return data

        loop = asyncio.get_running_loop()
        data = await loop.run_in_executor(self._fetch_pool, _fetch_tile_bytes, z, x, y)
        if data:
            self._write_cache(z, x, y, data)
        return data

    # ------------------------------------------------------------------
    # Background pre-fetch
    # ------------------------------------------------------------------

    def prefetch_location(self, lat: float, lon: float) -> None:
        """Queue a background download of ~2-mile radius tiles around (lat, lon).

        Deduplicates by rounding lat/lon to ~100 m so repeated calls for the
        same area don't double-fetch.
        """
        key = (round(lat, 3), round(lon, 3))
        with self._prefetch_lock:
            if key in self._prefetch_seen:
                return
            self._prefetch_seen.add(key)
        self._prefetch_pool.submit(self._prefetch_blocking, lat, lon)

    def _prefetch_blocking(self, lat: float, lon: float) -> None:
        if not _check_online():
            log.debug("tile prefetch skipped (%.4f, %.4f) — offline", lat, lon)
            return

        log.info("tile prefetch starting for (%.4f, %.4f)", lat, lon)
        fetched = 0
        for zoom in _PREFETCH_ZOOMS:
            for x, y in sorted(_tiles_in_radius(lat, lon, zoom)):
                if not self._path(zoom, x, y).exists():
                    data = _fetch_tile_bytes(zoom, x, y)
                    if data:
                        self._write_cache(zoom, x, y, data)
                        fetched += 1
                    time.sleep(_PREFETCH_DELAY_S)
        log.info("tile prefetch done for (%.4f, %.4f) — %d new tiles", lat, lon, fetched)
