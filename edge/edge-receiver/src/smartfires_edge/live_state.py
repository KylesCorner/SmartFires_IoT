import threading
import time
from collections import deque
from datetime import datetime, timezone
from typing import Any, Optional

from smartfires_edge.packet_loss import PacketLossTracker


class LiveState:
    """Thread-safe in-memory store feeding the web dashboard.

    Written to from the UART ingest thread (record_telemetry/record_status),
    read from the FastAPI request-handling thread(s). All access goes through
    a single lock since update/read rates here are low (a handful of
    packets/sec at most).
    """

    def __init__(
        self,
        node_ids: list[int],
        telemetry_maxlen: int = 2000,
        status_history_maxlen: int = 5000,
        reception_event_maxlen: int = 500,
    ) -> None:
        self._lock = threading.Lock()
        self._node_ids = list(node_ids)
        self.telemetry: dict[int, deque] = {n: deque(maxlen=telemetry_maxlen) for n in node_ids}
        self.status: dict[int, dict[str, Any]] = {}
        self.status_history: deque = deque(maxlen=status_history_maxlen)
        self.reception_events: dict[int, deque] = {
            n: deque(maxlen=reception_event_maxlen) for n in node_ids
        }
        self.tracker: Optional[PacketLossTracker] = None
        self._session_start_retx: dict[int, int] = {}
        self._session_start_fail: dict[int, int] = {}
        self._log_ring: deque[dict] = deque(maxlen=2000)
        self._log_lock = threading.Lock()
        self._log_total = 0

    def push_log(self, msg: str, node_id: int | None = None) -> None:
        with self._log_lock:
            self._log_ring.append({
                "t": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                "msg": msg,
                "node_id": node_id,
            })
            self._log_total += 1

    def drain_log(self, since_idx: int) -> tuple[list[dict], int]:
        """Return log entries after since_idx and the new cursor.

        Uses a monotonic total counter so the index stays valid across ring
        evictions — callers that trail behind get all currently-retained entries.
        """
        with self._log_lock:
            items = list(self._log_ring)
            total = self._log_total
        oldest_idx = total - len(items)
        if since_idx <= oldest_idx:
            return items, total
        return items[since_idx - oldest_idx:], total

    def _events_for(self, node_id: int) -> deque:
        return self.reception_events.setdefault(node_id, deque(maxlen=500))

    def record_telemetry(self, pkt: dict[str, Any]) -> None:
        node_id = int(pkt["node_id"])
        with self._lock:
            self.telemetry.setdefault(node_id, deque(maxlen=2000)).append(pkt)
            self._events_for(node_id).append((time.time(), int(pkt.get("seq", 0))))

    def record_status(self, status: dict[str, Any]) -> None:
        node_id = int(status["node_id"])
        now = time.time()
        with self._lock:
            entry = dict(status)
            entry["last_seen"] = now

            retx_total = status.get("retx_total")
            fail_total = status.get("fail_total")
            if isinstance(retx_total, int):
                self._session_start_retx.setdefault(node_id, retx_total)
                entry["retx_session"] = retx_total - self._session_start_retx[node_id]
            if isinstance(fail_total, int):
                self._session_start_fail.setdefault(node_id, fail_total)
                entry["fail_session"] = fail_total - self._session_start_fail[node_id]

            self.status[node_id] = entry
            self._events_for(node_id).append((now, int(status.get("seq", 0))))

            if status.get("gps_valid") and status.get("lat") not in ("", None):
                self.status_history.append(
                    {
                        "node_id": node_id,
                        "lat": status.get("lat"),
                        "lon": status.get("lon"),
                        "rssi": status.get("rssi"),
                        "ts": now,
                    }
                )

    def nodes_snapshot(self) -> dict[int, dict[str, Any]]:
        with self._lock:
            tracker_dict = self.tracker.to_dict() if self.tracker is not None else {"nodes": {}}
            tracker_nodes = tracker_dict.get("nodes", {})

            node_ids = set(self._node_ids) | set(self.status.keys())
            result: dict[int, dict[str, Any]] = {}
            for node_id in node_ids:
                status = self.status.get(node_id, {})
                tstats = tracker_nodes.get(str(node_id), {})
                expected = tstats.get("received", 0) + tstats.get("missing", 0)
                loss_percent = 100.0 * tstats.get("missing", 0) / expected if expected else 0.0
                result[node_id] = {
                    "node_id": node_id,
                    "lat": status.get("lat"),
                    "lon": status.get("lon"),
                    "battery_mv": status.get("battery_mv"),
                    "battery_pct": status.get("battery_pct"),
                    "rssi": status.get("rssi"),
                    "heading_deg": status.get("heading_deg"),
                    "retx_total": status.get("retx_total"),
                    "fail_total": status.get("fail_total"),
                    "retx_session": status.get("retx_session"),
                    "fail_session": status.get("fail_session"),
                    "last_seen": status.get("last_seen"),
                    "loss_percent": round(loss_percent, 2),
                    "received": tstats.get("received", 0),
                    "missing": tstats.get("missing", 0),
                }
            return result

    def reset(self) -> None:
        """Clear all live data buffers for a new session."""
        with self._lock:
            for q in self.telemetry.values():
                q.clear()
            self.status.clear()
            self.status_history.clear()
            for q in self.reception_events.values():
                q.clear()
            self._session_start_retx.clear()
            self._session_start_fail.clear()
        self.push_log("--- NEW SESSION ---", None)

    def status_history_snapshot(self, limit: int = 5000) -> list[dict[str, Any]]:
        with self._lock:
            items = list(self.status_history)
        return items[-limit:]

    def telemetry_recent(self, node_id: int, limit: int = 200) -> list[dict[str, Any]]:
        with self._lock:
            samples = list(self.telemetry.get(node_id, ()))
        return samples[-limit:]

    def reception_timeline(self, bins: int = 50, bin_width_s: float = 5.0) -> dict[int, list[int]]:
        now = time.time()
        window_start = now - bins * bin_width_s
        with self._lock:
            events_by_node = {n: list(events) for n, events in self.reception_events.items()}

        result: dict[int, list[int]] = {}
        for node_id, events in events_by_node.items():
            counts = [0] * bins
            for ts, _seq in events:
                if ts < window_start:
                    continue
                idx = int((ts - window_start) / bin_width_s)
                idx = max(0, min(bins - 1, idx))
                counts[idx] += 1
            result[node_id] = counts
        return result
