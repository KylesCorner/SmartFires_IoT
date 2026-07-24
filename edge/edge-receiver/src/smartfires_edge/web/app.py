import asyncio
import json
import queue
import socket
import threading
import time
from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.live_state import LiveState
from smartfires_edge.telemetry_cache import METRIC_KEYS, SessionTelemetryCache
from smartfires_edge.tile_cache import TileCache

STATIC_DIR = Path(__file__).parent / "static"
# Legacy manual tile directory — used as the default cache when no tile_cache_dir
# is supplied (preserves backward-compat with pre-loaded tile pyramids).
_LEGACY_TILES_DIR = Path(__file__).parent / "tiles"


class BaseStationPayload(BaseModel):
    lat: float
    lon: float


class CommandPayload(BaseModel):
    command: str


class NodeResetPayload(BaseModel):
    node_id: int


def _check_online() -> bool:
    """Return True if internet is reachable (TCP connect to Cloudflare DNS)."""
    try:
        s = socket.create_connection(("1.1.1.1", 443), timeout=3.0)
        s.close()
        return True
    except OSError:
        return False


def create_app(
    live_state: LiveState,
    data_dir: Path,
    base_station_store: Optional[BaseStationStore] = None,
    reset_event: Optional[threading.Event] = None,
    node_reset_queue: "Optional[queue.Queue[int]]" = None,
    tile_cache_dir: Optional[Path] = None,
    sniffer_enabled: bool = False,
) -> FastAPI:
    app = FastAPI(title="SmartFires Dashboard")
    store = base_station_store or BaseStationStore()
    tile_cache = TileCache(tile_cache_dir if tile_cache_dir is not None else _LEGACY_TILES_DIR)
    telemetry_cache = SessionTelemetryCache()

    def _current_session_csv() -> Optional[Path]:
        """The active session's CSV, if it has been written to yet.

        Uses live_state's session_log_dir (set on ingest startup and on every
        "New Session" reset) rather than globbing data_dir for the
        lexicographically last telemetry.csv — that glob would keep returning
        the *previous* session's file for as long as the new session dir has
        no telemetry.csv yet (the CSV is created lazily on first write_row),
        making the history-backed chart show stale/foreign session data right
        after a reset.
        """
        session_dir = live_state.session_log_dir()
        if session_dir is None:
            return None
        csv_path = session_dir / "telemetry.csv"
        return csv_path if csv_path.exists() else None

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

    @app.get("/live-log")
    def live_log_page() -> FileResponse:
        return FileResponse(STATIC_DIR / "live_log.html")

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

    @app.get("/api/server_time")
    def server_time() -> dict:
        """Wall-clock time on the Jetson, for display in the dashboard top bar."""
        return {"epoch_s": time.time()}

    @app.get("/api/session")
    def session() -> dict:
        """Current ingest session id, for display next to the clock in the top bar."""
        return live_state.session_info()

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
        start_ms: Optional[int] = None,
        end_ms: Optional[int] = None,
        max_points: int = 1500,
    ) -> dict:
        """CSV-backed series for the current session, decimated to at most
        ~2×max_points (each bucket keeps its min and max sample)."""
        if metric not in METRIC_KEYS:
            raise HTTPException(status_code=400, detail=f"Unknown metric {metric!r}")
        return telemetry_cache.history(
            _current_session_csv(), node, metric, start_ms, end_ms,
            max_points=min(max(max_points, 10), 4000),
        )

    @app.get("/api/session/timeline")
    def session_timeline(buckets: int = 300) -> dict:
        """Per-node activity counts from session start to now, with AWAKEN
        (node boot) markers — feeds the main page's session timeline."""
        info = live_state.session_info()
        session_start = info.get("session_start")
        return telemetry_cache.timeline(
            _current_session_csv(),
            buckets=min(max(buckets, 10), 2000),
            session_start_ms=int(session_start * 1000) if session_start else None,
        )

    @app.get("/api/awaken_events")
    def awaken_events(limit: int = 500) -> list[dict]:
        """This session's AWAKEN (node boot/reboot) events, newest first —
        feeds the Map & History page's reboot event table. Includes reset
        cause / hang-zone breadcrumb when the node firmware reports them
        (None on legacy pre-reset-diagnostics nodes)."""
        return telemetry_cache.awaken_events(
            _current_session_csv(), limit=min(max(limit, 1), 2000)
        )

    @app.get("/api/status_history")
    def status_history(limit: int = 5000) -> list[dict]:
        return live_state.status_history_snapshot(limit=limit)

    @app.get("/api/reception_timeline")
    def reception_timeline(bins: int = 50) -> dict:
        return live_state.reception_timeline(bins=bins)

    @app.get("/api/sniffer/stats")
    def sniffer_stats() -> dict:
        return live_state.sniffer_stats_snapshot()

    @app.get("/api/base_link")
    def get_base_link() -> dict:
        return live_state.link_status()

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

    @app.post("/api/node_reset")
    def node_reset(payload: NodeResetPayload) -> dict:
        if node_reset_queue is None:
            raise HTTPException(status_code=501, detail="Node reset not available")
        node_reset_queue.put(payload.node_id)
        return {"status": "reset_requested", "node_id": payload.node_id}

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
