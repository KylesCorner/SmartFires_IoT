import sys
import threading
import time
from collections import deque
from datetime import datetime, timezone

import serial

from smartfires_edge.ingest_service import (
    _send_time_sync,
    _time_sync_sender,
)
from smartfires_edge.packet import (
    PKT_AWAKEN,
    PKT_BUNDLE,
    PKT_FULL_STATE,
    PKT_STATUS,
)
from smartfires_edge.uart_receiver import iter_packets


def _fmt_num(value: object, digits: int = 2) -> str:
    if value in (None, ""):
        return ""
    try:
        return f"{float(value):.{digits}f}"
    except (TypeError, ValueError):
        return str(value)


def _fmt_int(value: object) -> str:
    if value in (None, ""):
        return ""
    try:
        return str(int(value))
    except (TypeError, ValueError):
        return str(value)


def _row_to_cells(row: dict, columns: list[tuple[str, str]]) -> list[str]:
    return [str(row.get(key, "")) for key, _ in columns]


def _print_table(title: str, columns: list[tuple[str, str]], rows: list[dict]) -> None:
    print(title)
    if not rows:
        print("  (no data yet)")
        print()
        return

    headers = [label for _, label in columns]
    cell_rows = [_row_to_cells(r, columns) for r in rows]
    widths = [len(h) for h in headers]

    for row in cell_rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))

    header = " | ".join(h.ljust(widths[i]) for i, h in enumerate(headers))
    sep = "-+-".join("-" * widths[i] for i in range(len(widths)))
    print(header)
    print(sep)
    for row in cell_rows:
        print(" | ".join(row[i].ljust(widths[i]) for i in range(len(widths))))
    print()


def _render_screen(
    port: str,
    baud: int,
    telemetry_rows: deque,
    status_by_node: dict[int, dict],
) -> None:
    print("\x1b[2J\x1b[H", end="")
    now = datetime.now(timezone.utc).isoformat(timespec="seconds")
    print(f"SmartFires Edge Visualizer  port={port} baud={baud}  now={now}")
    print("Press Ctrl+C to stop.\n")

    telemetry_columns = [
        ("sample_utc", "sample_utc"),
        ("node_id", "node"),
        ("seq", "seq"),
        ("temp_c", "temp_c"),
        ("humidity_pct", "humidity_pct"),
        ("wind_mps", "wind_mps"),
        ("pm1_0_ug_m3", "pm1.0"),
        ("pm2_5_ug_m3", "pm2.5"),
        ("pm4_0_ug_m3", "pm4.0"),
        ("pm10_ug_m3", "pm10"),
    ]
    _print_table("Telemetry Samples", telemetry_columns, list(telemetry_rows))

    status_rows = [status_by_node[node_id] for node_id in sorted(status_by_node.keys())]
    status_columns = [
        ("updated_utc", "updated_utc"),
        ("node_id", "node"),
        ("seq", "seq"),
        ("gps_valid", "gps_valid"),
        ("lat", "lat"),
        ("lon", "lon"),
        ("battery_valid", "batt_valid"),
        ("battery_mv", "batt_mv"),
        ("battery_pct", "batt_pct"),
        ("rssi", "rssi"),
    ]
    _print_table("Battery and Location", status_columns, status_rows)


def run_visualize(
    port: str,
    baud: int,
    sync_interval_s: int,
    telemetry_rows_max: int,
) -> int:
    telemetry_rows: deque = deque(maxlen=telemetry_rows_max)
    status_by_node: dict[int, dict] = {}

    sync_state = {"next_seq": 0}
    session_start = time.time()
    session_id = int(session_start * 1000) & 0xFFFFFFFF

    try:
        sync_thread_started = False
        write_lock = threading.Lock()

        for event, _receiver, ser in iter_packets(port, baud):
            if not sync_thread_started:
                sync_thread = threading.Thread(
                    target=_time_sync_sender,
                    args=(ser, write_lock, sync_state, session_id, session_start, sync_interval_s),
                    daemon=True,
                )
                sync_thread.start()
                sync_thread_started = True

            hdr_node = event.get("node_id")
            hdr_seq = event.get("seq")
            pkt_type = event.get("pkt_type")

            if hdr_node is not None and hdr_seq is not None and pkt_type == PKT_AWAKEN:
                _send_time_sync(
                    ser=ser,
                    write_lock=write_lock,
                    sync_state=sync_state,
                    session_id=session_id,
                    session_start=session_start,
                    reason="awaken",
                    trigger_node=int(hdr_node),
                    trigger_seq=int(hdr_seq),
                )
            elif hdr_node is not None and hdr_seq is not None and pkt_type is not None:
                if sync_state["next_seq"] == 0:
                    _send_time_sync(
                        ser=ser,
                        write_lock=write_lock,
                        sync_state=sync_state,
                        session_id=session_id,
                        session_start=session_start,
                        reason="visualize_start",
                        trigger_node=int(hdr_node),
                        trigger_seq=int(hdr_seq),
                    )

            status = event.get("status")
            if status:
                status_by_node[int(status.get("node_id"))] = {
                    "updated_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                    "node_id": _fmt_int(status.get("node_id")),
                    "seq": _fmt_int(status.get("seq")),
                    "gps_valid": str(status.get("gps_valid")),
                    "lat": _fmt_num(status.get("lat"), 7),
                    "lon": _fmt_num(status.get("lon"), 7),
                    "battery_valid": str(status.get("battery_valid")),
                    "battery_mv": _fmt_int(status.get("battery_mv")),
                    "battery_pct": _fmt_int(status.get("battery_pct")),
                    "rssi": _fmt_int(status.get("rssi")),
                }

            for pkt in event.get("packets", []):
                sample_ms = pkt.get("session_time_ms")
                sample_utc = ""
                if sample_ms not in (None, ""):
                    try:
                        sample_ts = session_start + (float(sample_ms) / 1000.0)
                        sample_utc = datetime.fromtimestamp(sample_ts, tz=timezone.utc).isoformat(
                            timespec="milliseconds"
                        )
                    except (TypeError, ValueError, OSError):
                        sample_utc = ""

                telemetry_rows.append(
                    {
                        "sample_utc": sample_utc,
                        "node_id": _fmt_int(pkt.get("node_id")),
                        "seq": _fmt_int(pkt.get("seq")),
                        "temp_c": _fmt_num(pkt.get("temp_c"), 2),
                        "humidity_pct": _fmt_num(pkt.get("humidity_pct"), 2),
                        "wind_mps": _fmt_num(pkt.get("wind_mps"), 2),
                        "pm1_0_ug_m3": _fmt_num(pkt.get("pm1_0_ug_m3"), 1),
                        "pm2_5_ug_m3": _fmt_num(pkt.get("pm2_5_ug_m3"), 1),
                        "pm4_0_ug_m3": _fmt_num(pkt.get("pm4_0_ug_m3"), 1),
                        "pm10_ug_m3": _fmt_num(pkt.get("pm10_ug_m3"), 1),
                    }
                )

            _render_screen(port, baud, telemetry_rows, status_by_node)

    except KeyboardInterrupt:
        print("\nStopped visualizer.")
        return 0
    except Exception as exc:
        print(f"\n[FATAL][VIS] {exc}", file=sys.stderr)
        return 1

