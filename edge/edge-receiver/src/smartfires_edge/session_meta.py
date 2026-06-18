from datetime import datetime, timezone
from pathlib import Path

from smartfires_edge.state_store import atomic_write_json


class SessionMetaLogger:
    """Writes and atomically updates a session manifest alongside the daily telemetry CSV.

    Output: data_dir/telemetry/session-YYYY-MM-DD.json

    The date in the filename matches the telemetry CSV for the same session, so a
    post-processing script can find the metadata for any CSV by substituting
    'telemetry-' → 'session-'.
    """

    def __init__(
        self,
        session_id: int,
        session_start: float,
        port: str,
        baud: int,
        data_dir: Path,
    ) -> None:
        self._session_id = session_id
        self._session_start = session_start
        self._port = port
        self._baud = baud
        self._node_registry: dict[str, dict] = {}
        self._path = data_dir / "session.json"
        self._write()

    def _write(self) -> None:
        payload = {
            "session_id": f"0x{self._session_id:08X}",
            "started_at": datetime.fromtimestamp(
                self._session_start, tz=timezone.utc
            ).isoformat(timespec="milliseconds"),
            "port": self._port,
            "baud": self._baud,
            "node_registry": self._node_registry,
        }
        atomic_write_json(self._path, payload)

    def on_awaken(self, node_id: int, uid_hash: int) -> None:
        key = str(node_id)
        if key not in self._node_registry:
            self._node_registry[key] = {
                "uid_hash": f"0x{uid_hash:08x}",
                "first_seen": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            }
            self._write()
