import copy
import importlib
import threading
import time
from pathlib import Path
from typing import Any

from smartfires_edge.state_store import atomic_write_json, read_json


class SessionManager:
    def __init__(self, path: Path | None = None) -> None:
        self._path = path or (Path.home() / ".smartfires" / "session.json")
        self._lock = threading.Lock()
        self._state = self._default_state()
        self.load()

    def _default_state(self) -> dict[str, Any]:
        return {
            "node_id_to_uid_hash": {},
            "uid_hash_to_node_id": {},
            "command_queue": [],
            "node_status": {},
            "last_updated": int(time.time()),
        }

    @staticmethod
    def _uid_key(uid_hash: int) -> str:
        return f"0x{uid_hash:08X}"

    @staticmethod
    def _parse_uid_key(uid_key: str) -> int:
        if uid_key.startswith("0x") or uid_key.startswith("0X"):
            return int(uid_key, 16)
        return int(uid_key)

    def load(self) -> None:
        with self._lock:
            raw = read_json(self._path)
            if not raw:
                self._state = self._default_state()
                return

            node_map_raw  = raw.get("node_id_to_uid_hash", {})
            uid_map_raw   = raw.get("uid_hash_to_node_id", {})
            status_raw    = raw.get("node_status", {})

            self._state = {
                "node_id_to_uid_hash": {
                    int(node_id): self._parse_uid_key(str(uid_hash))
                    for node_id, uid_hash in node_map_raw.items()
                },
                "uid_hash_to_node_id": {
                    self._parse_uid_key(str(uid_hash)): int(node_id)
                    for uid_hash, node_id in uid_map_raw.items()
                },
                "command_queue": raw.get("command_queue", []),
                "node_status": {
                    int(node_id): value
                    for node_id, value in status_raw.items()
                },
                "last_updated": int(raw.get("last_updated", time.time())),
            }

    def save(self) -> None:
        with self._lock:
            self._save_locked()

    def _save_locked(self) -> None:
        payload = {
            "node_id_to_uid_hash": {
                str(node_id): self._uid_key(uid_hash)
                for node_id, uid_hash in self._state["node_id_to_uid_hash"].items()
            },
            "uid_hash_to_node_id": {
                self._uid_key(uid_hash): node_id
                for uid_hash, node_id in self._state["uid_hash_to_node_id"].items()
            },
            "command_queue": self._state["command_queue"],
            "node_status": {
                str(node_id): value
                for node_id, value in self._state["node_status"].items()
            },
            "last_updated": int(time.time()),
        }
        atomic_write_json(self._path, payload)

    def get_uid_hash_for_node(self, node_id: int) -> int | None:
        with self._lock:
            return self._state["node_id_to_uid_hash"].get(int(node_id))

    def on_awaken(self, node_id: int, uid_hash: int) -> dict[str, Any]:
        with self._lock:
            node_id  = int(node_id)
            uid_hash = int(uid_hash)
            self._state["node_id_to_uid_hash"][node_id]  = uid_hash
            self._state["uid_hash_to_node_id"][uid_hash] = node_id
            node_status = self._state["node_status"].setdefault(node_id, {})
            node_status["last_seen"] = int(time.time())
            self._save_locked()
            return {"node_id": node_id, "uid_hash": uid_hash}

    def mark_node_seen(self, node_id: int) -> None:
        with self._lock:
            node_id = int(node_id)
            node_status = self._state["node_status"].setdefault(node_id, {})
            node_status["last_seen"] = int(time.time())

    def on_cmd_ack(self, node_id: int, uid_hash: int, cmd_type: int, status: int) -> dict[str, Any]:
        with self._lock:
            node_id  = int(node_id)
            uid_hash = int(uid_hash)
            if uid_hash != 0:
                self._state["node_id_to_uid_hash"][node_id]  = uid_hash
                self._state["uid_hash_to_node_id"][uid_hash] = node_id

            entry = {
                "type": int(cmd_type),
                "node_id": node_id,
                "uid_hash": uid_hash,
                "sent_at": int(time.time()),
                "acked": True,
                "status": int(status),
            }
            self._state["command_queue"].append(entry)
            if len(self._state["command_queue"]) > 256:
                self._state["command_queue"] = self._state["command_queue"][-256:]
            self._save_locked()
            return entry

    def on_status(self, node_id: int, uid_hash: int | None, status: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            node_id = int(node_id)
            if uid_hash is None:
                uid_hash = self._state["node_id_to_uid_hash"].get(node_id)
            elif uid_hash != 0:
                uid_hash = int(uid_hash)
                self._state["node_id_to_uid_hash"][node_id]  = uid_hash
                self._state["uid_hash_to_node_id"][uid_hash] = node_id

            node_status = self._state["node_status"].setdefault(node_id, {})
            node_status["last_seen"] = int(time.time())

            heading_deg = status.get("heading_deg")
            if status.get("imu_valid") and heading_deg != "" and heading_deg is not None:
                heading_true_deg = float(heading_deg)
                node_status["heading_true_deg"] = heading_true_deg
                raw_acc = status.get("heading_accuracy")
                if raw_acc != "" and raw_acc is not None:
                    node_status["heading_accuracy_deg"] = round(int(raw_acc) / 4096.0, 2)
                corrected_heading = self._location_corrected_heading(
                    heading_true_deg=heading_true_deg,
                    lat=status.get("lat"),
                    lon=status.get("lon"),
                    gps_valid=bool(status.get("gps_valid")),
                )
                if corrected_heading is not None:
                    node_status["location_corrected_heading"] = corrected_heading
                node_status["last_heading_ts"] = int(time.time())
                return {
                    "computed": True,
                    "heading_true_deg": heading_true_deg,
                    "location_corrected_heading": corrected_heading,
                }

            return {"computed": False}

    @staticmethod
    def _location_corrected_heading(
        heading_true_deg: float,
        lat: Any,
        lon: Any,
        gps_valid: bool,
    ) -> float | None:
        if not gps_valid or lat in ("", None) or lon in ("", None):
            return None

        try:
            geomag_module = importlib.import_module("geomag")
        except ImportError:
            return None

        try:
            lat_f = float(lat)
            lon_f = float(lon)
            declination_deg = float(geomag_module.declination(lat_f, lon_f))
        except (TypeError, ValueError, AttributeError):
            return None

        return round((heading_true_deg + declination_deg) % 360.0, 1)

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._state)
