import queue
import threading

import uvicorn

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.config import EdgeConfig
from smartfires_edge.ingest_service import run_receive
from smartfires_edge.live_state import LiveState
from smartfires_edge.sniffer_service import run_sniffer
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
    reset_event = threading.Event()
    node_reset_queue: queue.Queue[int] = queue.Queue()
    tx_power_queue: queue.Queue[dict] = queue.Queue()

    ingest_thread = threading.Thread(
        target=run_receive,
        kwargs=dict(
            cfg=cfg.ingest,
            live_state=live_state,
            log_fn=live_state.push_log,
            reset_event=reset_event,
            node_reset_queue=node_reset_queue,
            tx_power_queue=tx_power_queue,
        ),
        daemon=True,
    )
    ingest_thread.start()

    if cfg.ingest.sniffer.enabled:
        sniffer_thread = threading.Thread(
            target=run_sniffer,
            kwargs=dict(
                cfg=cfg.ingest.sniffer,
                live_state=live_state,
                log_fn=live_state.push_log,
            ),
            daemon=True,
        )
        sniffer_thread.start()

    app = create_app(
        live_state=live_state,
        data_dir=cfg.ingest.data_dir,
        base_station_store=BaseStationStore(),
        reset_event=reset_event,
        node_reset_queue=node_reset_queue,
        tx_power_queue=tx_power_queue,
        # Namespaced by tile source: switching providers (as happened when we
        # moved off raw OSM tiles to CARTO Voyager) must not silently mix old
        # and new tiles under the same path — bump this name on any future
        # source change instead of relying on a manual cache purge.
        tile_cache_dir=cfg.ingest.data_dir / "tiles" / "carto-voyager",
        sniffer_enabled=cfg.ingest.sniffer.enabled,
    )
    uvicorn.run(app, host=cfg.web_host, port=cfg.web_http_port, log_level="info")
    return 0
