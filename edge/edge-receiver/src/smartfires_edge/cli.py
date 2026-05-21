import argparse
import curses
import queue
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any

import serial

from smartfires_edge.packet import (
    PKT_CALIBRATION_DATA,
    PKT_CMD_ACK,
    PKT_CMD_CALIBRATE,
    PKT_CMD_RESET,
    encode_cmd_calibrate_frame,
    encode_cmd_reset_frame,
)
from smartfires_edge.session import SessionManager
from smartfires_edge.uart_receiver import FrameReceiver


HELP_TEXT: dict[str, str] = {
    "calibrate": "calibrate node <id> | cal <id>",
    "reset": "reset node <id> [hard]",
    "list": "list nodes | list calibrations",
    "save": "save session",
    "load": "load session",
    "clear": "clear calibration <id> | clear calibrations",
    "help": "help [command]",
    "quit": "quit | exit",
}


@dataclass
class PendingCommand:
    node_id: int
    cmd_type: int
    sent_at: float
    ack_warned: bool = False
    duration_s: int = 60
    waiting_calibration: bool = False
    calibration_deadline: float = 0.0


class CliRuntime:
    def __init__(self) -> None:
        self.stop_event = threading.Event()
        self.ui_events: "queue.Queue[str]" = queue.Queue()
        self.command_queue: "queue.Queue[str]" = queue.Queue()
        self.write_lock = threading.Lock()
        self.state_lock = threading.Lock()

        self.serial_conn: serial.Serial | None = None
        self.pending_commands: list[PendingCommand] = []
        self.next_seq = 0

    def next_command_seq(self) -> int:
        with self.state_lock:
            seq = self.next_seq
            self.next_seq = (self.next_seq + 1) & 0xFF
            return seq

    def log(self, message: str) -> None:
        ts = datetime.utcnow().strftime("%H:%M:%S")
        self.ui_events.put(f"[{ts}] {message}")

    def add_pending(self, pending: PendingCommand) -> None:
        with self.state_lock:
            self.pending_commands.append(pending)

    def mark_cmd_ack(self, node_id: int, cmd_type: int) -> PendingCommand | None:
        with self.state_lock:
            for pending in reversed(self.pending_commands):
                if pending.node_id == node_id and pending.cmd_type == cmd_type and not pending.ack_warned:
                    pending.ack_warned = True
                    if cmd_type == PKT_CMD_CALIBRATE:
                        pending.waiting_calibration = True
                        pending.calibration_deadline = time.time() + pending.duration_s + 15.0
                    return pending
        return None

    def complete_calibration_wait(self, node_id: int) -> None:
        with self.state_lock:
            for pending in self.pending_commands:
                if pending.node_id == node_id and pending.waiting_calibration:
                    pending.waiting_calibration = False

    def collect_timeout_warnings(self) -> list[str]:
        warnings: list[str] = []
        now = time.time()
        with self.state_lock:
            for pending in self.pending_commands:
                if not pending.ack_warned and (now - pending.sent_at) >= 5.0:
                    pending.ack_warned = True
                    warnings.append(
                        f"[Warning] No CMD_ACK from node {pending.node_id} after 5s for cmd=0x{pending.cmd_type:02x}."
                    )
                if pending.waiting_calibration and now >= pending.calibration_deadline:
                    pending.waiting_calibration = False
                    warnings.append(
                        f"[Warning] No CALIBRATION_DATA from node {pending.node_id} within timeout."
                    )
        return warnings


def _format_nodes(session: SessionManager) -> list[str]:
    snap = session.snapshot()
    lines = ["node_id uid_hash last_seen calib heading"]
    node_map = snap.get("node_id_to_uid_hash", {})
    node_status = snap.get("node_status", {})
    calibrations = snap.get("calibrations", {})

    for node_id in sorted(node_map.keys()):
        uid_hash = node_map[node_id]
        status = node_status.get(node_id, {})
        last_seen = status.get("last_seen", "--")
        heading = status.get("heading_true_deg", "--")
        calib = "valid" if uid_hash in calibrations else "none"
        lines.append(
            f"{node_id:<7} 0x{uid_hash:08x} {last_seen!s:<9} {calib:<5} {heading}"
        )
    if len(lines) == 1:
        lines.append("(no nodes seen)")
    return lines


def _format_calibrations(session: SessionManager) -> list[str]:
    snap = session.snapshot()
    lines = ["uid_hash sample_count timestamp eigenvalues"]
    calibrations = snap.get("calibrations", {})
    if not calibrations:
        lines.append("(no calibrations)")
        return lines

    for uid_hash in sorted(calibrations.keys()):
        calib = calibrations[uid_hash]
        eig = calib.get("eigenvalues", [])
        eig_txt = ",".join(f"{float(v):.3f}" for v in eig)
        lines.append(
            f"0x{uid_hash:08x} {calib.get('sample_count', 0):<12} {calib.get('timestamp', '--'):<10} [{eig_txt}]"
        )
    return lines


def _process_command(line: str, runtime: CliRuntime, session: SessionManager) -> None:
    tokens = line.strip().split()
    if not tokens:
        return

    cmd = tokens[0].lower()
    if cmd in ("quit", "exit"):
        runtime.log("Shutting down...")
        runtime.stop_event.set()
        return

    if cmd == "help":
        if len(tokens) > 1:
            key = tokens[1].lower()
            runtime.log(HELP_TEXT.get(key, "Unknown command."))
        else:
            runtime.log("Commands: " + " | ".join(HELP_TEXT.values()))
        return

    if cmd == "list":
        if len(tokens) < 2:
            runtime.log("Usage: list nodes | list calibrations")
            return
        target = tokens[1].lower()
        lines = _format_nodes(session) if target == "nodes" else _format_calibrations(session)
        for out in lines:
            runtime.log(out)
        return

    if cmd == "save" and len(tokens) == 2 and tokens[1].lower() == "session":
        session.save()
        runtime.log("Session saved.")
        return

    if cmd == "load" and len(tokens) == 2 and tokens[1].lower() == "session":
        session.load()
        runtime.log("Session loaded.")
        return

    if cmd == "clear":
        if len(tokens) >= 2 and tokens[1].lower() == "calibrations":
            session.clear_calibrations()
            runtime.log("All calibrations cleared.")
            return
        if len(tokens) == 3 and tokens[1].lower() == "calibration":
            node_id = int(tokens[2])
            ok = session.clear_calibration_by_node(node_id)
            runtime.log("Calibration cleared." if ok else "No calibration found for node.")
            return
        runtime.log("Usage: clear calibration <id> | clear calibrations")
        return

    if cmd in ("cal", "calibrate"):
        if (cmd == "cal" and len(tokens) != 2) or (cmd == "calibrate" and len(tokens) != 3):
            runtime.log("Usage: calibrate node <id> | cal <id>")
            return

        node_id = int(tokens[-1])
        uid_hash = session.get_uid_hash_for_node(node_id)
        if uid_hash is None:
            runtime.log(f"[Error] Node {node_id} not found. Use 'list nodes'.")
            return

        seq = runtime.next_command_seq()
        frame = encode_cmd_calibrate_frame(node_id=node_id, duration_s=60, seq=seq)
        if runtime.serial_conn is None:
            runtime.log("[Error] Serial not ready yet.")
            return
        with runtime.write_lock:
            runtime.serial_conn.write(frame)

        runtime.add_pending(
            PendingCommand(node_id=node_id, cmd_type=PKT_CMD_CALIBRATE, sent_at=time.time(), duration_s=60)
        )
        runtime.log(f"[Sent] CMD_CALIBRATE node={node_id} uid=0x{uid_hash:08x} seq={seq}")
        return

    if cmd == "reset":
        if len(tokens) < 3 or tokens[1].lower() != "node":
            runtime.log("Usage: reset node <id> [hard]")
            return

        node_id = int(tokens[2])
        reset_type = 1 if len(tokens) > 3 and tokens[3].lower() == "hard" else 0
        seq = runtime.next_command_seq()
        frame = encode_cmd_reset_frame(node_id=node_id, reset_type=reset_type, seq=seq)
        if runtime.serial_conn is None:
            runtime.log("[Error] Serial not ready yet.")
            return
        with runtime.write_lock:
            runtime.serial_conn.write(frame)

        runtime.add_pending(
            PendingCommand(node_id=node_id, cmd_type=PKT_CMD_RESET, sent_at=time.time())
        )
        runtime.log(f"[Sent] CMD_RESET node={node_id} reset_type={reset_type} seq={seq}")
        return

    runtime.log("[Error] Invalid command. Type 'help'.")


def _listener_worker(port: str, baud: int, runtime: CliRuntime, session: SessionManager) -> None:
    receiver = FrameReceiver()
    try:
        with serial.Serial(port, baud, timeout=0.25) as ser:
            runtime.serial_conn = ser
            runtime.log(f"Serial connected {port} @ {baud}")

            while not runtime.stop_event.is_set():
                raw = ser.read(1)
                if not raw:
                    continue

                event = receiver.push_byte(raw[0])
                if event is None:
                    continue

                pkt_type = event.get("pkt_type")
                node_id = event.get("node_id")
                seq = event.get("seq")
                runtime.log(f"RX type=0x{pkt_type:02x} node={node_id} seq={seq} rssi={event.get('rssi')}")

                awaken = event.get("awaken")
                if awaken:
                    aw = session.on_awaken(int(awaken["node_id"]), int(awaken["uid_hash"]))
                    msg = "calibration on file" if aw["has_calibration"] else "no calibration"
                    runtime.log(
                        f"AWAKEN node={aw['node_id']} uid=0x{aw['uid_hash']:08x} {msg}"
                    )

                status = event.get("status")
                if status:
                    uid_hash = session.get_uid_hash_for_node(int(status["node_id"]))
                    heading = session.on_status(int(status["node_id"]), uid_hash, status)
                    if heading.get("computed"):
                        runtime.log(
                            f"STATUS node={status['node_id']} heading={heading['heading_true_deg']} pitch={heading['pitch_deg']} roll={heading['roll_deg']}"
                        )

                cmd_ack = event.get("cmd_ack")
                if cmd_ack:
                    session.on_cmd_ack(
                        node_id=int(cmd_ack["node_id"]),
                        uid_hash=int(cmd_ack["uid_hash"]),
                        cmd_type=int(cmd_ack["cmd_type"]),
                        status=int(cmd_ack["status"]),
                    )
                    pending = runtime.mark_cmd_ack(int(cmd_ack["node_id"]), int(cmd_ack["cmd_type"]))
                    runtime.log(
                        f"CMD_ACK node={cmd_ack['node_id']} cmd=0x{cmd_ack['cmd_type']:02x} status={cmd_ack['status']}"
                    )
                    if pending and pending.waiting_calibration:
                        runtime.log(
                            f"Node {pending.node_id} acknowledged calibration; waiting up to {pending.duration_s + 15}s for CALIBRATION_DATA"
                        )

                calibration_data = event.get("calibration_data")
                if calibration_data:
                    runtime.complete_calibration_wait(int(calibration_data["node_id"]))
                    result = session.on_calibration_data(
                        node_id=int(calibration_data["node_id"]),
                        uid_hash=int(calibration_data["uid_hash"]),
                        stats=calibration_data,
                    )
                    runtime.log(
                        f"CALIBRATION_DATA node={calibration_data['node_id']} samples={calibration_data['sample_count']} accepted={result.get('accepted')}"
                    )
    except Exception as exc:
        runtime.log(f"[FATAL] Listener error: {exc}")
        runtime.stop_event.set()


def _command_worker(runtime: CliRuntime, session: SessionManager) -> None:
    while not runtime.stop_event.is_set():
        try:
            line = runtime.command_queue.get(timeout=0.2)
            _process_command(line, runtime, session)
        except queue.Empty:
            pass

        for warning in runtime.collect_timeout_warnings():
            runtime.log(warning)


def _run_curses_ui(stdscr: Any, runtime: CliRuntime) -> None:
    curses.curs_set(1)
    stdscr.nodelay(True)
    stdscr.timeout(100)

    log_lines: deque[str] = deque(maxlen=1000)
    input_line = ""

    while not runtime.stop_event.is_set():
        while True:
            try:
                log_lines.append(runtime.ui_events.get_nowait())
            except queue.Empty:
                break

        height, width = stdscr.getmaxyx()
        split = max(3, int(height * 0.8))

        stdscr.erase()
        stdscr.addstr(0, 0, "SMARTFIRES JETSON CLI")
        stdscr.hline(1, 0, ord("="), max(0, width - 1))

        visible = split - 3
        start = max(0, len(log_lines) - visible)
        for i, line in enumerate(list(log_lines)[start : start + visible]):
            stdscr.addnstr(2 + i, 0, line, max(0, width - 1))

        stdscr.hline(split, 0, ord("-"), max(0, width - 1))
        prompt = f"> {input_line}"
        stdscr.addnstr(split + 1, 0, prompt, max(0, width - 1))
        stdscr.refresh()

        ch = stdscr.getch()
        if ch == -1:
            continue
        if ch in (10, 13):
            runtime.command_queue.put(input_line)
            input_line = ""
        elif ch in (127, curses.KEY_BACKSPACE, 8):
            input_line = input_line[:-1]
        elif 32 <= ch < 127:
            input_line += chr(ch)


def run_cli(
    port: str,
    baud: int,
    session_file: Path | None = None,
) -> int:
    runtime = CliRuntime()
    session = SessionManager(path=session_file)

    listener = threading.Thread(target=_listener_worker, args=(port, baud, runtime, session), daemon=True)
    command = threading.Thread(target=_command_worker, args=(runtime, session), daemon=True)
    listener.start()
    command.start()

    runtime.log("Type 'help' for commands. Type 'quit' to exit.")

    try:
        curses.wrapper(lambda stdscr: _run_curses_ui(stdscr, runtime))
    except KeyboardInterrupt:
        runtime.stop_event.set()
    except Exception as exc:
        runtime.log(f"[FATAL] UI error: {exc}")
        runtime.stop_event.set()

    runtime.stop_event.set()
    listener.join(timeout=1.5)
    command.join(timeout=1.5)

    session.save()
    return 0


def build_cli_parser(subparsers: argparse._SubParsersAction) -> None:
    cli = subparsers.add_parser("cli", help="Interactive Jetson CLI")
    cli.add_argument("--port", default="/dev/ttyTHS1")
    cli.add_argument("--baud", type=int, default=115200)
    cli.add_argument("--session-file", type=Path, default=None)
