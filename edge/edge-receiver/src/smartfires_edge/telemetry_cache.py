"""Incremental in-memory index of the current session's telemetry.csv.

The dashboard's history and session-timeline endpoints need whole-session
views, but LiveState's ring buffer only holds the last ~25 minutes of samples.
Re-parsing a multi-hour CSV on every request is too slow, so this cache tails
the file: it remembers its byte offset and parses only the bytes appended
since the previous request. Switching to a different session file (or the
file shrinking, i.e. being rewritten) resets the cache.

All timestamps in telemetry.csv are UTC; telemetry rows carry a trailing "Z"
while status/awaken rows don't. Everything here is epoch milliseconds.
"""

import csv
import io
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

METRIC_KEYS = (
    "temp_c",
    "humidity_pct",
    "wind_mps",
    "pm1_0_ug_m3",
    "pm2_5_ug_m3",
    "pm4_0_ug_m3",
    "pm10_ug_m3",
)

# Fixed resolution of the always-maintained per-node activity index; the
# timeline endpoint aggregates these into whatever display bucket size the
# client asks for. 15 s ≈ one telemetry bundle + one status packet.
ACTIVITY_BUCKET_MS = 15_000


def _ts_ms(raw: str) -> Optional[int]:
    try:
        d = datetime.fromisoformat(raw.rstrip("Z"))
    except ValueError:
        return None
    return int(d.replace(tzinfo=timezone.utc).timestamp() * 1000)


def _int_or_none(raw: str) -> Optional[int]:
    try:
        return int(raw)
    except ValueError:
        return None


class SessionTelemetryCache:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._path: Optional[Path] = None
        self._offset = 0
        self._carry = b""  # trailing partial line from the last read
        self._col_idx: Optional[dict[str, int]] = None
        # node -> [(t_ms, temp, humidity, wind, pm1_0, pm2_5, pm4_0, pm10)]
        self._samples: dict[int, list[tuple]] = {}
        # node -> {absolute ACTIVITY_BUCKET_MS bucket index -> row count}
        self._activity: dict[int, dict[int, int]] = {}
        self._awaken: list[dict] = []  # see _ingest_row's "awaken" branch for shape
        self._first_ms: Optional[int] = None

    # ------------------------------------------------------------------
    # File tailing
    # ------------------------------------------------------------------

    def _reset(self, path: Optional[Path]) -> None:
        self._path = path
        self._offset = 0
        self._carry = b""
        self._col_idx = None
        self._samples = {}
        self._activity = {}
        self._awaken = []
        self._first_ms = None

    def _refresh(self, path: Optional[Path]) -> None:
        if path != self._path:
            self._reset(path)
        if path is None:
            return
        try:
            size = path.stat().st_size
        except OSError:
            return
        if size < self._offset:  # rewritten/truncated
            self._reset(path)
        if size == self._offset:
            return
        try:
            with open(path, "rb") as f:
                f.seek(self._offset)
                chunk = f.read()
                self._offset = f.tell()
        except OSError:
            return

        data = self._carry + chunk
        nl = data.rfind(b"\n")
        if nl < 0:
            self._carry = data
            return
        self._carry = data[nl + 1 :]
        text = data[:nl].decode("utf-8", errors="replace")

        for parts in csv.reader(io.StringIO(text)):
            if not parts:
                continue
            if self._col_idx is None:
                if parts[0] == "timestamp":
                    self._col_idx = {name: i for i, name in enumerate(parts)}
                continue
            self._ingest_row(parts)

    def _ingest_row(self, parts: list[str]) -> None:
        idx = self._col_idx
        assert idx is not None
        try:
            t = _ts_ms(parts[idx["timestamp"]])
            node = int(parts[idx["node_id"]])
            ptype = parts[idx["packet_type"]]
        except (KeyError, ValueError, IndexError):
            return
        if t is None:
            return

        if self._first_ms is None or t < self._first_ms:
            self._first_ms = t
        buckets = self._activity.setdefault(node, {})
        b = t // ACTIVITY_BUCKET_MS
        buckets[b] = buckets.get(b, 0) + 1

        if ptype == "telemetry":
            vals: list[Optional[float]] = [t]
            try:
                for key in METRIC_KEYS:
                    raw = parts[idx[key]]
                    vals.append(float(raw) if raw else None)
            except (KeyError, ValueError, IndexError):
                return
            self._samples.setdefault(node, []).append(tuple(vals))
        elif ptype == "awaken":
            def _col(name: str) -> str:
                i = idx.get(name)
                return parts[i] if i is not None and i < len(parts) else ""

            names_raw = _col("reset_cause_names")
            self._awaken.append({
                "t_ms": t,
                "node_id": node,
                "seq": _int_or_none(_col("seq")),
                "rssi": _int_or_none(_col("rssi")),
                # None on legacy (9-byte, uid_hash-only) AWAKEN frames — see
                # packet.decode_awaken.
                "reset_cause": _int_or_none(_col("reset_cause")),
                "reset_cause_names": names_raw.split("|") if names_raw else None,
                "hang_zone": _int_or_none(_col("hang_zone")),
                "hang_zone_name": _col("hang_zone_name") or None,
            })

    # ------------------------------------------------------------------
    # Queries
    # ------------------------------------------------------------------

    def history(
        self,
        path: Optional[Path],
        node_id: int,
        metric: str,
        start_ms: Optional[int],
        end_ms: Optional[int],
        max_points: int,
    ) -> dict:
        """Windowed, decimated series for one node+metric.

        Decimation keeps each bucket's min and max sample (not the mean) so
        short-lived anomalies — single-sample temperature spikes, dropouts —
        survive at any zoom level.
        """
        value_idx = METRIC_KEYS.index(metric) + 1  # slot 0 is the timestamp
        with self._lock:
            self._refresh(path)
            samples = self._samples.get(node_id, [])
            if start_ms is None:
                start_ms = self._first_ms if self._first_ms is not None else 0
            if end_ms is None:
                end_ms = int(time.time() * 1000)
            span = max(end_ms - start_ms, 1)
            bucket_ms = max(span // max(max_points, 1), 1)

            # bucket index -> [t_at_min, min, t_at_max, max]
            buckets: dict[int, list] = {}
            for s in samples:
                t = s[0]
                if t < start_ms or t > end_ms:
                    continue
                v = s[value_idx]
                if v is None:
                    continue
                bi = (t - start_ms) // bucket_ms
                b = buckets.get(bi)
                if b is None:
                    buckets[bi] = [t, v, t, v]
                else:
                    if v < b[1]:
                        b[0], b[1] = t, v
                    if v > b[3]:
                        b[2], b[3] = t, v

            points: list[list] = []
            for bi in sorted(buckets):
                t_min, v_min, t_max, v_max = buckets[bi]
                if t_min == t_max:
                    points.append([t_min, v_min])
                else:
                    pair = [[t_min, v_min], [t_max, v_max]]
                    pair.sort()
                    points.extend(pair)
            return {"points": points, "bucket_ms": bucket_ms}

    def timeline(
        self,
        path: Optional[Path],
        buckets: int,
        session_start_ms: Optional[int] = None,
    ) -> dict:
        """Per-node activity counts from session start to now, plus AWAKEN
        (node boot) markers so restarts — e.g. watchdog resets — are visible."""
        with self._lock:
            self._refresh(path)
            now_ms = int(time.time() * 1000)
            start = session_start_ms if session_start_ms is not None else self._first_ms
            if start is None:
                start = now_ms
            end = max(now_ms, start + 1)
            bucket_ms = max((end - start + buckets - 1) // buckets, 1)
            count = (end - start) // bucket_ms + 1

            nodes: dict[str, list[int]] = {}
            for node, act in self._activity.items():
                counts = [0] * count
                for ab, c in act.items():
                    t = ab * ACTIVITY_BUCKET_MS
                    if t < start - ACTIVITY_BUCKET_MS or t > end:
                        continue
                    di = min(max((t - start) // bucket_ms, 0), count - 1)
                    counts[di] += c
                nodes[str(node)] = counts

            awaken = [
                [e["t_ms"], e["node_id"]] for e in self._awaken
                if start <= e["t_ms"] <= end
            ]
            return {
                "start_ms": start,
                "end_ms": end,
                "bucket_ms": bucket_ms,
                "nodes": nodes,
                "awaken": awaken,
            }

    def awaken_events(self, path: Optional[Path], limit: int = 500) -> list[dict]:
        """This session's AWAKEN (node boot/reboot) events, newest first, with
        reset cause and hang-zone breadcrumb when the node firmware reports
        them — feeds the Map & History page's reboot event table."""
        with self._lock:
            self._refresh(path)
            events = sorted(self._awaken, key=lambda e: e["t_ms"], reverse=True)
            return events[: max(limit, 1)]
