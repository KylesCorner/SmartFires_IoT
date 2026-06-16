import threading
from pathlib import Path

import uvicorn

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.ingest_service import run_receive
from smartfires_edge.live_state import LiveState
from smartfires_edge.web.app import create_app


def run_web(
    port: str,
    baud: int,
    data_dir: Path,
    nodes: list[int],
    metrics_interval_s: int,
    sync_interval_s: int,
    fsync_every_row: bool,
    raw_log: bool,
    anemometer_port: str | None,
    anemometer_baud: int,
    anemometer_address: int,
    anemometer_interval_s: float,
    host: str,
    http_port: int,
) -> int:
    live_state = LiveState(nodes)

    ingest_thread = threading.Thread(
        target=run_receive,
        kwargs=dict(
            port=port,
            baud=baud,
            data_dir=data_dir,
            nodes=nodes,
            metrics_interval_s=metrics_interval_s,
            sync_interval_s=sync_interval_s,
            fsync_every_row=fsync_every_row,
            raw_log=raw_log,
            anemometer_port=anemometer_port,
            anemometer_baud=anemometer_baud,
            anemometer_address=anemometer_address,
            anemometer_interval_s=anemometer_interval_s,
            live_state=live_state,
        ),
        daemon=True,
    )
    ingest_thread.start()

    app = create_app(live_state=live_state, data_dir=data_dir, base_station_store=BaseStationStore())
    uvicorn.run(app, host=host, port=http_port, log_level="info")
    return 0
