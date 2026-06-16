import csv
from datetime import datetime
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.live_state import LiveState

STATIC_DIR = Path(__file__).parent / "static"
TILES_DIR = Path(__file__).parent / "tiles"

TELEMETRY_METRICS = {
    "temp_c",
    "humidity_pct",
    "wind_mps",
    "pm1_0_ug_m3",
    "pm2_5_ug_m3",
    "pm4_0_ug_m3",
    "pm10_ug_m3",
}


class BaseStationPayload(BaseModel):
    lat: float
    lon: float


def _read_telemetry_history(
    telemetry_dir: Path,
    node_id: int,
    metric: str,
    start: Optional[str],
    end: Optional[str],
) -> list[dict]:
    start_dt = datetime.fromisoformat(start) if start else None
    end_dt = datetime.fromisoformat(end) if end else None
    results: list[dict] = []

    if not telemetry_dir.exists():
        return results

    for path in sorted(telemetry_dir.glob("telemetry-*.csv")):
        with open(path, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                if row.get("packet_type") != "telemetry":
                    continue
                try:
                    if int(row.get("node_id") or -1) != node_id:
                        continue
                except ValueError:
                    continue

                ts_raw = row.get("timestamp")
                if not ts_raw:
                    continue
                try:
                    ts = datetime.fromisoformat(ts_raw)
                except ValueError:
                    continue
                if start_dt and ts < start_dt:
                    continue
                if end_dt and ts > end_dt:
                    continue

                value = row.get(metric)
                if value in (None, ""):
                    continue
                try:
                    results.append({"timestamp": ts_raw, "value": float(value)})
                except ValueError:
                    continue

    return results


def create_app(
    live_state: LiveState,
    data_dir: Path,
    base_station_store: Optional[BaseStationStore] = None,
) -> FastAPI:
    app = FastAPI(title="SmartFires Dashboard")
    store = base_station_store or BaseStationStore()
    telemetry_dir = data_dir / "telemetry"

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")
    if TILES_DIR.exists() and any(TILES_DIR.iterdir()):
        app.mount("/tiles", StaticFiles(directory=TILES_DIR), name="tiles")

    @app.get("/")
    def index() -> FileResponse:
        return FileResponse(STATIC_DIR / "index.html")

    @app.get("/map-history")
    def map_history() -> FileResponse:
        return FileResponse(STATIC_DIR / "map_history.html")

    @app.get("/api/nodes")
    def get_nodes() -> dict:
        return live_state.nodes_snapshot()

    @app.get("/api/telemetry/recent")
    def telemetry_recent(node: int, limit: int = 200) -> list[dict]:
        return live_state.telemetry_recent(node, limit=limit)

    @app.get("/api/telemetry/history")
    def telemetry_history(
        node: int,
        metric: str,
        start: Optional[str] = None,
        end: Optional[str] = None,
    ) -> list[dict]:
        if metric not in TELEMETRY_METRICS:
            raise HTTPException(status_code=400, detail=f"Unknown metric {metric!r}")
        return _read_telemetry_history(telemetry_dir, node, metric, start, end)

    @app.get("/api/status_history")
    def status_history(limit: int = 5000) -> list[dict]:
        return live_state.status_history_snapshot(limit=limit)

    @app.get("/api/reception_timeline")
    def reception_timeline(bins: int = 50, bin_width_s: float = 5.0) -> dict:
        return live_state.reception_timeline(bins=bins, bin_width_s=bin_width_s)

    @app.get("/api/base_station")
    def get_base_station() -> dict:
        return store.get() or {}

    @app.post("/api/base_station")
    def set_base_station(payload: BaseStationPayload) -> dict:
        return store.set(payload.lat, payload.lon)

    return app
