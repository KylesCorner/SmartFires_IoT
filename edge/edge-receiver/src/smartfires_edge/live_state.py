import statistics
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
        self._sniffer_ring: deque[dict] = deque(maxlen=5000)
        self._sniffer_lock = threading.Lock()
        self._sniffer_total = 0
        self._sniffer_stats: dict[int, dict[str, Any]] = {}
        self._base_debug_ring: deque[dict] = deque(maxlen=5000)
        self._base_debug_lock = threading.Lock()
        self._base_debug_total = 0

    def push_log(
        self,
        msg: str,
        node_id: int | None = None,
        source: str = "ingest",
        kind: str = "other",
    ) -> None:
        with self._log_lock:
            self._log_ring.append({
                "t": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                "msg": msg,
                "node_id": node_id,
                "source": source,
                "kind": kind,
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

    def push_base_debug(self, record: dict[str, Any]) -> None:
        with self._base_debug_lock:
            self._base_debug_ring.append(record)
            self._base_debug_total += 1

    def drain_base_debug(self, since_idx: int) -> tuple[list[dict], int]:
        """Same monotonic-cursor pattern as drain_log."""
        with self._base_debug_lock:
            items = list(self._base_debug_ring)
            total = self._base_debug_total
        oldest_idx = total - len(items)
        if since_idx <= oldest_idx:
            return items, total
        return items[since_idx - oldest_idx:], total

    def push_sniffer_event(self, event: dict[str, Any]) -> None:
        with self._sniffer_lock:
            self._sniffer_ring.append(event)
            self._sniffer_total += 1

            node_id = event.get("node_id")
            if node_id is None or event.get("pkt_type") == "TIME_SYNC":
                return
            stats = self._sniffer_stats.setdefault(
                node_id,
                {
                    "packets": 0,
                    "rssi_sum": 0.0,
                    "rssi_count": 0,
                    "snr_sum": 0.0,
                    "snr_count": 0,
                    "jitter_samples": deque(maxlen=200),
                    "guard_violations": 0,
                },
            )
            stats["packets"] += 1
            if event.get("rssi") is not None:
                stats["rssi_sum"] += event["rssi"]
                stats["rssi_count"] += 1
            if event.get("snr") is not None:
                stats["snr_sum"] += event["snr"]
                stats["snr_count"] += 1
            if event.get("jitter_ms") is not None:
                stats["jitter_samples"].append(event["jitter_ms"])
            if event.get("guard_violation"):
                stats["guard_violations"] += 1

    def drain_sniffer(self, since_idx: int) -> tuple[list[dict], int]:
        """Same monotonic-cursor pattern as drain_log."""
        with self._sniffer_lock:
            items = list(self._sniffer_ring)
            total = self._sniffer_total
        oldest_idx = total - len(items)
        if since_idx <= oldest_idx:
            return items, total
        return items[since_idx - oldest_idx:], total

    def sniffer_stats_snapshot(self) -> dict[int, dict[str, Any]]:
        with self._sniffer_lock:
            result: dict[int, dict[str, Any]] = {}
            for node_id, s in self._sniffer_stats.items():
                jitter = list(s["jitter_samples"])
                result[node_id] = {
                    "node_id": node_id,
                    "packets": s["packets"],
                    "avg_rssi": round(s["rssi_sum"] / s["rssi_count"], 1) if s["rssi_count"] else None,
                    "avg_snr": round(s["snr_sum"] / s["snr_count"], 1) if s["snr_count"] else None,
                    "jitter_std_ms": round(statistics.pstdev(jitter), 1) if len(jitter) >= 2 else None,
                    "guard_violations": s["guard_violations"],
                }
            return result

    def reset_sniffer_stats(self) -> None:
        """Called by sniffer_service when a new base-station session is detected."""
        with self._sniffer_lock:
            self._sniffer_stats.clear()

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
                    "duplicates": tstats.get("duplicates", 0),
                    "last_rssi": tstats.get("last_rssi"),
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
        with self._sniffer_lock:
            self._sniffer_ring.clear()
            self._sniffer_stats.clear()
        self.push_log("--- NEW SESSION ---", None, kind="session")

    def status_history_snapshot(self, limit: int = 5000) -> list[dict[str, Any]]:
        with self._lock:
            items = list(self.status_history)
        return items[-limit:]

    def telemetry_recent(self, node_id: int, limit: int = 200) -> list[dict[str, Any]]:
        with self._lock:
            samples = list(self.telemetry.get(node_id, ()))
        return samples[-limit:]

    def reception_timeline(self, bins: int = 50) -> dict[int, dict]:
        """Return a seq-space sliding window for each node.

        Each entry in ``bins`` corresponds to one sequence number slot,
        ordered oldest-left to newest-right, with the rightmost slot always
        being ``last_seq``.  State is one of:
          ``"received"``  – seq in seen_seqs
          ``"missing"``   – seq in [first_seq, last_seq] but not received
          ``"before"``    – seq predates first_seq (no expectation)
        """
        with self._lock:
            if self.tracker is None:
                return {}
            result: dict[int, dict] = {}
            for node_id, stats in self.tracker.nodes.items():
                if stats.first_seq is None:
                    # No packets yet — fill with placeholders so the grid renders.
                    result[node_id] = {
                        "last_seq": None,
                        "bins": [{"seq": None, "state": "before"} for _ in range(bins)],
                    }
                    continue

                last_seq = stats.last_seq
                first_seq = stats.first_seq
                span = (last_seq - first_seq) & 0xFF  # inclusive distance

                slot_list = []
                for i in range(bins):
                    seq = (last_seq - (bins - 1 - i)) & 0xFF
                    dist_from_first = (seq - first_seq) & 0xFF
                    if dist_from_first > span:
                        state = "before"
                    elif seq in stats.seen_seqs:
                        state = "received"
                    else:
                        state = "missing"
                    slot_list.append({"seq": seq, "state": state})

                result[node_id] = {"last_seq": last_seq, "bins": slot_list}
            return result
