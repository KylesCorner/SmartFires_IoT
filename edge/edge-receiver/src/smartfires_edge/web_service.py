import os
import queue
import subprocess
import threading
from pathlib import Path

import uvicorn

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.config import EdgeConfig
from smartfires_edge.ingest_service import run_receive
from smartfires_edge.live_state import LiveState
from smartfires_edge.sniffer_service import run_sniffer
from smartfires_edge.web.app import create_app


def _current_git_commit(repo_root: Path) -> str | None:
    """Return the commit backing the active checkout, if it is available."""
    try:
        completed = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            text=True,
            capture_output=True,
            timeout=2,
            check=True,
        )
    except (OSError, subprocess.SubprocessError):
        return None

    commit = completed.stdout.strip()
    return commit if commit else None


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
    # One event represents an intentional whole-process restart.  The HTTP
    # handler sets it after returning its acknowledgement; the supervisor
    # below then stops uvicorn and the service manager starts a fresh process.
    reset_event = threading.Event()
    ingest_failed = threading.Event()
    node_reset_queue: queue.Queue[int] = queue.Queue()
    tx_power_queue: queue.Queue[dict] = queue.Queue()

    def ingest_runner() -> None:
        rc = run_receive(
            cfg=cfg.ingest,
            live_state=live_state,
            log_fn=live_state.push_log,
            reset_event=reset_event,
            stop_event=reset_event,
            node_reset_queue=node_reset_queue,
            tx_power_queue=tx_power_queue,
        )
        # A required worker that exits unexpectedly must take the process down
        # rather than leaving an apparently healthy dashboard with stale data.
        if rc != 0 and not reset_event.is_set():
            ingest_failed.set()
            reset_event.set()

    ingest_thread = threading.Thread(
        target=ingest_runner,
        name="smartfires-ingest",
        # The main thread supervises and joins this worker. Keeping it daemon
        # is the final bounded-shutdown escape hatch if a filesystem/driver
        # call ignores the five-second join deadline.
        daemon=True,
    )
    ingest_thread.start()

    sniffer_thread = None
    if cfg.ingest.sniffer.enabled:
        sniffer_thread = threading.Thread(
            target=run_sniffer,
            kwargs=dict(
                cfg=cfg.ingest.sniffer,
                live_state=live_state,
                log_fn=live_state.push_log,
                stop_event=reset_event,
            ),
            name="smartfires-sniffer",
            daemon=True,
        )
        sniffer_thread.start()

    repo_root = Path.cwd()
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
        # A dashboard-triggered update is enabled only under systemd, where a
        # clean process exit is guaranteed to launch the newly installed
        # package again. Direct development launches remain read-only.
        system_update_command=(
            repo_root / "edge" / "smartfires-manager.sh"
            if os.environ.get("INVOCATION_ID")
            else None
        ),
        current_commit=_current_git_commit(repo_root),
    )
    config = uvicorn.Config(
        app,
        host=cfg.web_host,
        port=cfg.web_http_port,
        log_level="info",
        # Idle dashboard WebSockets spend most of their lifetime waiting in
        # application loops and may not observe the browser disconnect while
        # Uvicorn is shutting down. Without a deadline, New Session and the
        # dashboard updater can remain forever at "Waiting for background
        # tasks to complete", preventing systemd from launching the next
        # process. After this bounded grace period Uvicorn cancels those idle
        # connection tasks and the normal worker joins below can finish.
        timeout_graceful_shutdown=5.0,
    )
    server = uvicorn.Server(config)

    def supervise() -> None:
        while not server.should_exit:
            if reset_event.wait(0.1):
                server.should_exit = True
                return
            if not ingest_thread.is_alive():
                live_state.push_log(
                    "[FATAL] required ingest worker exited; shutting down",
                    None, kind="error",
                )
                ingest_failed.set()
                reset_event.set()
                server.should_exit = True
                return

    monitor = threading.Thread(target=supervise, name="smartfires-supervisor", daemon=True)
    monitor.start()
    try:
        server.run()
    finally:
        reset_event.set()
        ingest_thread.join(timeout=5.0)
        if ingest_thread.is_alive():
            live_state.push_log("[FATAL] ingest worker did not stop before deadline", None, kind="error")
        if sniffer_thread is not None:
            sniffer_thread.join(timeout=2.0)
    # Intentional New Session exits cleanly for a restart-capable supervisor;
    # unexpected ingest failure remains non-zero for supervisors without one.
    return 1 if ingest_failed.is_set() else 0
