import threading

import uvicorn

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.config import EdgeConfig
from smartfires_edge.ingest_service import run_receive
from smartfires_edge.live_state import LiveState
from smartfires_edge.web.app import create_app


def run_web(cfg: EdgeConfig) -> int:
    """Run the UART ingest loop and the live web dashboard concurrently.

    The ingest loop runs in a background thread; the FastAPI/uvicorn server
    runs on the main thread (uvicorn takes over the event loop).

    Args:
        cfg: Top-level config sourced from :class:`~smartfires_edge.config.EdgeConfig`.
             Web-specific settings are in ``cfg.web_host`` / ``cfg.web_http_port``;
             ingest settings are in ``cfg.ingest``.
    """
    live_state = LiveState(cfg.ingest.nodes)

    ingest_thread = threading.Thread(
        target=run_receive,
        kwargs=dict(cfg=cfg.ingest, live_state=live_state),
        daemon=True,
    )
    ingest_thread.start()

    app = create_app(
        live_state=live_state,
        data_dir=cfg.ingest.data_dir,
        base_station_store=BaseStationStore(),
    )
    uvicorn.run(app, host=cfg.web_host, port=cfg.web_http_port, log_level="info")
    return 0
