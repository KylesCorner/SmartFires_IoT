import asyncio
import csv
import json
import socket
import threading
from datetime import datetime
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.live_state import LiveState
from smartfires_edge.tile_cache import TileCache

STATIC_DIR = Path(__file__).parent / "static"
# Legacy manual tile directory — used as the default cache when no tile_cache_dir
# is supplied (preserves backward-compat with pre-loaded tile pyramids).
_LEGACY_TILES_DIR = Path(__file__).parent / "tiles"

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


class CommandPayload(BaseModel):
    command: str


def _check_online() -> bool:
    """Return True if internet is reachable (TCP connect to Cloudflare DNS)."""
    try:
        s = socket.create_connection(("1.1.1.1", 443), timeout=3.0)
        s.close()
        return True
    except OSError:
        return False


def _read_telemetry_history(
    data_dir: Path,
    node_id: int,
    metric: str,
    start: Optional[str],
    end: Optional[str],
) -> list[dict]:
    start_dt = datetime.fromisoformat(start) if start else None
    end_dt = datetime.fromisoformat(end) if end else None
    results: list[dict] = []

    if not data_dir.exists():
        return results

    for path in sorted(data_dir.glob("*/telemetry.csv")):
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
    reset_event: Optional[threading.Event] = None,
    tile_cache_dir: Optional[Path] = None,
    sniffer_enabled: bool = False,
) -> FastAPI:
    app = FastAPI(title="SmartFires Dashboard")
    store = base_station_store or BaseStationStore()
    tile_cache = TileCache(tile_cache_dir if tile_cache_dir is not None else _LEGACY_TILES_DIR)

    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

    # ------------------------------------------------------------------
    # Pages
    # ------------------------------------------------------------------

    @app.get("/")
    def index() -> FileResponse:
        return FileResponse(STATIC_DIR / "index.html")

    @app.get("/map-history")
    def map_history() -> FileResponse:
        return FileResponse(STATIC_DIR / "map_history.html")

    @app.get("/sniffer")
    def sniffer_page() -> FileResponse:
        return FileResponse(STATIC_DIR / "sniffer.html")

    @app.get("/debug")
    def debug_page() -> FileResponse:
        return FileResponse(STATIC_DIR / "debug.html")

    # ------------------------------------------------------------------
    # Tile cache endpoints
    #
    # The browser (not the Jetson) fetches tiles from OSM and uploads them
    # here. GET serves the cache; PUT stores what the browser fetched.
    # ------------------------------------------------------------------

    @app.head("/tiles/{z}/{x}/{y}.png")
    def head_tile(z: int, x: int, y: int) -> Response:
        if tile_cache.has(z, x, y):
            return Response(
                status_code=200,
                media_type="image/png",
                headers={"Cache-Control": "public, max-age=86400"},
            )
        raise HTTPException(status_code=404, detail="Tile not cached")

    @app.get("/tiles/{z}/{x}/{y}.png")
    def get_tile(z: int, x: int, y: int) -> Response:
        data = tile_cache.get(z, x, y)
        if data is None:
            raise HTTPException(status_code=404, detail="Tile not cached")
        return Response(
            content=data,
            media_type="image/png",
            headers={"Cache-Control": "public, max-age=86400"},
        )

    @app.put("/tiles/{z}/{x}/{y}.png")
    async def put_tile(z: int, x: int, y: int, request: Request) -> dict:
        data = await request.body()
        if data:
            tile_cache.put(z, x, y, data)
        return {"status": "ok"}

    # ------------------------------------------------------------------
    # Connectivity probe
    # ------------------------------------------------------------------

    @app.get("/api/connectivity")
    async def connectivity() -> dict:
        loop = asyncio.get_running_loop()
        online = await loop.run_in_executor(None, _check_online)
        return {"online": online}

    # ------------------------------------------------------------------
    # Data API
    # ------------------------------------------------------------------

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
        return _read_telemetry_history(data_dir, node, metric, start, end)

    @app.get("/api/status_history")
    def status_history(limit: int = 5000) -> list[dict]:
        return live_state.status_history_snapshot(limit=limit)

    @app.get("/api/reception_timeline")
    def reception_timeline(bins: int = 50) -> dict:
        return live_state.reception_timeline(bins=bins)

    @app.get("/api/sniffer/stats")
    def sniffer_stats() -> dict:
        return live_state.sniffer_stats_snapshot()

    @app.get("/api/base_station")
    def get_base_station() -> dict:
        return store.get() or {}

    @app.post("/api/base_station")
    def set_base_station(payload: BaseStationPayload) -> dict:
        return store.set(payload.lat, payload.lon)

    @app.post("/api/command")
    def post_command(payload: CommandPayload) -> dict:
        return {"status": "queued", "command": payload.command}

    @app.post("/api/new_session")
    def new_session() -> dict:
        if reset_event is None:
            raise HTTPException(status_code=501, detail="Session reset not available")
        reset_event.set()
        return {"status": "reset_requested"}

    @app.websocket("/ws/log")
    async def websocket_log(ws: WebSocket) -> None:
        await ws.accept()
        idx = 0
        try:
            while True:
                entries, idx = live_state.drain_log(idx)
                for entry in entries:
                    await ws.send_text(json.dumps(entry))
                await asyncio.sleep(0.05)
        except (WebSocketDisconnect, Exception):
            pass

    @app.websocket("/ws/base-debug")
    async def websocket_base_debug(ws: WebSocket) -> None:
        await ws.accept()
        idx = 0
        try:
            while True:
                entries, idx = live_state.drain_base_debug(idx)
                for entry in entries:
                    await ws.send_text(json.dumps(entry))
                await asyncio.sleep(0.05)
        except (WebSocketDisconnect, Exception):
            pass

    @app.websocket("/ws/sniffer")
    async def websocket_sniffer(ws: WebSocket) -> None:
        await ws.accept()
        if not sniffer_enabled:
            await ws.send_text(json.dumps({"event": "not_configured"}))
            await ws.close()
            return
        idx = 0
        try:
            while True:
                entries, idx = live_state.drain_sniffer(idx)
                for entry in entries:
                    await ws.send_text(json.dumps(entry))
                await asyncio.sleep(0.05)
        except (WebSocketDisconnect, Exception):
            pass

    return app
