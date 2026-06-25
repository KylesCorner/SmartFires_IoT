#!/usr/bin/env python3
"""
SmartFires structured serial debug viewer.

Reads firmware log lines like:

  @SFDBG	v=1	node=1	src=sht31	lvl=I	seq=42	t=123456	msg=sampled

Displays:
  - grouped recent logs by node/src
  - raw recent lines
  - parse/error count
  - optional log file capture

Requires:
  pip install pyserial

Example:
  python tools/sf_debug_viewer.py --port /dev/ttyACM0 --baud 115200
  python tools/sf_debug_viewer.py --port /dev/ttyUSB0 --src sht31 --min-level D
  python tools/sf_debug_viewer.py --port COM5 --baud 115200
"""

from __future__ import annotations

import argparse
import curses
import datetime as dt
import os
import queue
import signal
import sys
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Deque, Dict, Iterable, Optional, Tuple

import serial

LEVEL_RANK = {
    "T": 0,
    "D": 1,
    "I": 2,
    "W": 3,
    "E": 4,
    "O": 5,
}


@dataclass(frozen=True)
class LogRecord:
    recv_time: dt.datetime
    node: str
    src: str
    lvl: str
    seq: Optional[int]
    t_ms: Optional[int]
    msg: str
    raw: str


@dataclass
class ViewerState:
    records_by_key: Dict[Tuple[str, str], Deque[LogRecord]]
    raw_lines: Deque[str]
    parse_errors: int = 0
    total_records: int = 0
    total_raw_lines: int = 0
    running: bool = True


def unescape_field(s: str) -> str:
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt == "t":
                out.append("\t")
            elif nxt == "n":
                out.append("\n")
            elif nxt == "r":
                out.append("\r")
            elif nxt == "\\":
                out.append("\\")
            else:
                out.append(nxt)
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def parse_sfdbg_line(line: str) -> Optional[LogRecord]:
    line = line.rstrip("\r\n")

    if not line.startswith("@SFDBG\t"):
        return None

    fields = {}
    parts = line.split("\t")[1:]

    for part in parts:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        fields[key] = unescape_field(value)

    node = fields.get("node", "?")
    src = fields.get("src", "?")
    lvl = fields.get("lvl", "?")
    msg = fields.get("msg", "")

    seq = None
    t_ms = None

    try:
        if "seq" in fields:
            seq = int(fields["seq"])
    except ValueError:
        seq = None

    try:
        if "t" in fields:
            t_ms = int(fields["t"])
    except ValueError:
        t_ms = None

    return LogRecord(
        recv_time=dt.datetime.now(dt.timezone.utc).astimezone(),
        node=node,
        src=src,
        lvl=lvl,
        seq=seq,
        t_ms=t_ms,
        msg=msg,
        raw=line,
    )


def level_allows(record_lvl: str, min_level: str) -> bool:
    return LEVEL_RANK.get(record_lvl, 99) >= LEVEL_RANK.get(min_level, 0)


def serial_reader(
    port: str,
    baud: int,
    output_queue: "queue.Queue[str]",
    stop_event: threading.Event,
) -> None:
    while not stop_event.is_set():
        try:
            with serial.Serial(port, baudrate=baud, timeout=0.2) as ser:
                while not stop_event.is_set():
                    raw = ser.readline()
                    if not raw:
                        continue

                    try:
                        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    except Exception:
                        line = repr(raw)

                    output_queue.put(line)

        except serial.SerialException as exc:
            output_queue.put(f"@VIEWER\tserial_error={exc}")
            time.sleep(1.0)


def append_log_file(path: Optional[Path], line: str) -> None:
    if path is None:
        return

    path.parent.mkdir(parents=True, exist_ok=True)
    timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
    with path.open("a", encoding="utf-8") as f:
        f.write(f"{timestamp}\t{line}\n")


def format_record(r: LogRecord) -> str:
    wall = r.recv_time.strftime("%H:%M:%S")
    seq = "-" if r.seq is None else str(r.seq)
    t_ms = "-" if r.t_ms is None else str(r.t_ms)
    return f"{wall} [{r.lvl}] node={r.node} src={r.src} seq={seq} t={t_ms}  {r.msg}"


def safe_addstr(win, y: int, x: int, text: str, attr: int = 0) -> None:
    try:
        h, w = win.getmaxyx()
        if y < 0 or y >= h or x >= w:
            return
        win.addstr(y, x, text[: max(0, w - x - 1)], attr)
    except curses.error:
        pass


def draw_ui(
    stdscr, state: ViewerState, selected_keys: Optional[set[Tuple[str, str]]]
) -> None:
    stdscr.erase()
    height, width = stdscr.getmaxyx()

    title = (
        "SmartFires Debug Viewer  "
        f"records={state.total_records} raw={state.total_raw_lines} parse_errors={state.parse_errors}  "
        "q=quit"
    )
    safe_addstr(stdscr, 0, 0, title, curses.A_BOLD)

    keys = sorted(state.records_by_key.keys(), key=lambda k: (k[0], k[1]))

    left_w = min(32, max(24, width // 4))
    right_x = left_w + 1

    safe_addstr(stdscr, 2, 0, "Streams", curses.A_BOLD)
    y = 3
    for key in keys[: max(0, height - 5)]:
        if selected_keys and key not in selected_keys:
            continue

        records = state.records_by_key[key]
        last_lvl = records[-1].lvl if records else "?"
        label = f"node={key[0]} src={key[1]} ({len(records)}) [{last_lvl}]"
        safe_addstr(stdscr, y, 0, label)
        y += 1

    for row in range(1, height):
        safe_addstr(stdscr, row, left_w, "│")

    safe_addstr(stdscr, 2, right_x, "Recent grouped logs", curses.A_BOLD)

    y = 3
    max_lines = max(0, height - 8)

    grouped_recent = []
    for key in keys:
        if selected_keys and key not in selected_keys:
            continue
        grouped_recent.extend(list(state.records_by_key[key])[-5:])

    grouped_recent.sort(key=lambda r: r.recv_time)

    for rec in grouped_recent[-max_lines:]:
        attr = 0
        if rec.lvl == "E":
            attr = curses.A_BOLD
        elif rec.lvl == "W":
            attr = curses.A_STANDOUT

        safe_addstr(stdscr, y, right_x, format_record(rec), attr)
        y += 1

    raw_y = max(3, height - 5)
    safe_addstr(stdscr, raw_y, right_x, "Recent raw lines", curses.A_BOLD)

    y = raw_y + 1
    for line in list(state.raw_lines)[-3:]:
        safe_addstr(stdscr, y, right_x, line)
        y += 1

    stdscr.refresh()


def run_curses(
    stdscr,
    args: argparse.Namespace,
    input_queue: "queue.Queue[str]",
    stop_event: threading.Event,
) -> None:
    curses.curs_set(0)
    stdscr.nodelay(True)
    stdscr.timeout(100)

    selected_keys = None
    if args.node or args.src:
        selected_keys = set()

    state = ViewerState(
        records_by_key=defaultdict(lambda: deque(maxlen=args.per_stream)),
        raw_lines=deque(maxlen=args.raw_lines),
    )

    log_path = Path(args.log_file).expanduser() if args.log_file else None

    while state.running and not stop_event.is_set():
        try:
            while True:
                line = input_queue.get_nowait()
                state.total_raw_lines += 1
                state.raw_lines.append(line)
                append_log_file(log_path, line)

                rec = parse_sfdbg_line(line)
                if rec is None:
                    if line.startswith("@SFDBG"):
                        state.parse_errors += 1
                    continue

                if args.node and rec.node != args.node:
                    continue

                if args.src and rec.src != args.src:
                    continue

                if not level_allows(rec.lvl, args.min_level):
                    continue

                key = (rec.node, rec.src)
                state.records_by_key[key].append(rec)
                state.total_records += 1

        except queue.Empty:
            pass

        ch = stdscr.getch()
        if ch in (ord("q"), ord("Q")):
            state.running = False
            stop_event.set()
            break

        draw_ui(stdscr, state, selected_keys)
        time.sleep(0.05)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="SmartFires structured serial debug viewer")
    p.add_argument(
        "--port",
        required=True,
        help="Serial port, e.g. /dev/ttyACM0, /dev/ttyUSB0, COM5",
    )
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--node", help="Only show one node id, e.g. 1")
    p.add_argument("--src", help="Only show one source/module id, e.g. sht31")
    p.add_argument("--min-level", default="T", choices=["T", "D", "I", "W", "E"])
    p.add_argument("--per-stream", type=int, default=100)
    p.add_argument("--raw-lines", type=int, default=50)
    p.add_argument("--log-file", help="Optional raw capture file")
    return p


def main() -> int:
    args = build_arg_parser().parse_args()

    input_queue: "queue.Queue[str]" = queue.Queue()
    stop_event = threading.Event()

    def handle_signal(signum, frame):
        stop_event.set()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    reader = threading.Thread(
        target=serial_reader,
        args=(args.port, args.baud, input_queue, stop_event),
        daemon=True,
    )
    reader.start()

    curses.wrapper(run_curses, args, input_queue, stop_event)

    stop_event.set()
    reader.join(timeout=1.0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
