"""Disk-backed map tile cache — storage only, no HTTP fetching.

The Jetson never contacts OSM directly (OSM prohibits server-side tile proxying).
Tiles are fetched by the browser and uploaded here via PUT /tiles/{z}/{x}/{y}.png.
GET /tiles/{z}/{x}/{y}.png serves cached tiles to the browser for offline use.
Pre-fetching is also driven from the browser (see static/js/tile_layer.js).
"""

from __future__ import annotations

from pathlib import Path


class TileCache:
    """Simple disk cache for XYZ map tiles.

    Cache layout: ``{cache_dir}/{z}/{x}/{y}.png``.
    """

    def __init__(self, cache_dir: Path) -> None:
        self._dir = cache_dir
        self._dir.mkdir(parents=True, exist_ok=True)

    def _path(self, z: int, x: int, y: int) -> Path:
        return self._dir / str(z) / str(x) / f"{y}.png"

    def get(self, z: int, x: int, y: int) -> bytes | None:
        p = self._path(z, x, y)
        return p.read_bytes() if p.exists() else None

    def put(self, z: int, x: int, y: int, data: bytes) -> None:
        p = self._path(z, x, y)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)

    def has(self, z: int, x: int, y: int) -> bool:
        return self._path(z, x, y).exists()
