"""
Tunable configuration for the SmartFires edge receiver.

Single source of truth for all runtime defaults.  Every constant here maps
directly to a CLI flag default; the config/ header pattern on the firmware
side is mirrored here for the Jetson side.

Precedence for each setting (highest wins):
  1. Explicit CLI flag
  2. JSON config file (--config PATH)
  3. Constants defined in this file

Usage pattern in main.py::

    add_common_ingest_args(recv)      # --port, --baud, --data-dir, …
    add_anemometer_args(recv)         # --anemometer-port, …
    args = parser.parse_args()
    cfg = EdgeConfig.from_args(args, subcommand=args.command)
    run_receive(cfg.ingest, live_state=…)
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Default values — change here, takes effect everywhere
# ---------------------------------------------------------------------------

# Serial link to the base station — native USB CDC, not the Jetson's hardware
# UART (see documentation/Current_Architecture/UART_JETSON_BRIDGE.md). Use the
# udev-assigned stable symlink, not a raw /dev/ttyACM* path, since the base
# and sniffer Feathers enumerate identically and their ttyACM index can swap
# on reconnect/reboot.
DEFAULT_PORT: str = "/dev/smartfires-base"
DEFAULT_BAUD: int = 115200

# Data persistence (receive, web)
DEFAULT_DATA_DIR: Path = Path("/mnt/nvme_drive/data")

# Tracked nodes for packet-loss metrics (receive, web)
DEFAULT_NODES: list[int] = [1, 2]

# Ingest loop cadences (receive, web)
DEFAULT_METRICS_INTERVAL_S: int = 10    # how often packet-loss state is flushed to disk
DEFAULT_SYNC_INTERVAL_S: int = 600      # periodic TIME_SYNC broadcast interval (10 min)

# Anemometer — ES-W302 external wind sensor (receive, web; optional)
DEFAULT_ANEMOMETER_PORT: str | None = None  # None = disabled
DEFAULT_ANEMOMETER_BAUD: int = 9600
DEFAULT_ANEMOMETER_ADDRESS: int = 1
DEFAULT_ANEMOMETER_INTERVAL_S: float = 1.0  # Modbus poll interval

# Web dashboard (web subcommand only)
DEFAULT_WEB_HOST: str = "0.0.0.0"
DEFAULT_WEB_HTTP_PORT: int = 8080

# Passive LoRa sniffer — second USB-serial Feather, TDMA visualization (web subcommand only)
DEFAULT_SNIFFER_PORT: str | None = None  # None = sniffer disabled
DEFAULT_SNIFFER_BAUD: int = 115200
# Must match the NUM_SLOTS build flag baked into the deployed node Feathers
# (see platformio/platformio.ini and the root CLAUDE.md "must match" note).
DEFAULT_NUM_SLOTS: int = 4

# Visualize (visualize subcommand only)
DEFAULT_TELEMETRY_ROWS: int = 20

# CLI runtime timeouts (cli subcommand only)
CLI_CMD_ACK_TIMEOUT_S: float = 5.0   # warn if no CMD_ACK received within this window
CLI_CALIBRATION_DURATION_S: int = 60  # default duration sent in CMD_CALIBRATE


# ---------------------------------------------------------------------------
# Config dataclasses
# ---------------------------------------------------------------------------

@dataclass
class AnemometerConfig:
    """Optional ES-W302 external anemometer settings."""

    port: str | None = DEFAULT_ANEMOMETER_PORT
    baud: int = DEFAULT_ANEMOMETER_BAUD
    address: int = DEFAULT_ANEMOMETER_ADDRESS
    interval_s: float = DEFAULT_ANEMOMETER_INTERVAL_S

    @property
    def enabled(self) -> bool:
        return self.port is not None


@dataclass
class SnifferConfig:
    """Optional passive LoRa sniffer settings (second USB-serial Feather)."""

    port: str | None = DEFAULT_SNIFFER_PORT
    baud: int = DEFAULT_SNIFFER_BAUD
    num_slots: int = DEFAULT_NUM_SLOTS

    @property
    def enabled(self) -> bool:
        return self.port is not None


@dataclass
class IngestConfig:
    """UART ingest loop settings — used by ``receive`` and ``web`` subcommands."""

    port: str = DEFAULT_PORT
    baud: int = DEFAULT_BAUD
    data_dir: Path = field(default_factory=lambda: Path(DEFAULT_DATA_DIR))
    nodes: list[int] = field(default_factory=lambda: list(DEFAULT_NODES))
    metrics_interval_s: int = DEFAULT_METRICS_INTERVAL_S
    sync_interval_s: int = DEFAULT_SYNC_INTERVAL_S
    fsync_every_row: bool = False
    raw_log: bool = False
    anemometer: AnemometerConfig = field(default_factory=AnemometerConfig)
    sniffer: SnifferConfig = field(default_factory=SnifferConfig)


@dataclass
class EdgeConfig:
    """Top-level config assembled by :meth:`from_args` from CLI args + optional JSON file."""

    ingest: IngestConfig = field(default_factory=IngestConfig)
    # Web-specific
    web_host: str = DEFAULT_WEB_HOST
    web_http_port: int = DEFAULT_WEB_HTTP_PORT
    # Visualize-specific
    telemetry_rows: int = DEFAULT_TELEMETRY_ROWS

    @classmethod
    def from_args(cls, args: Any, *, subcommand: str) -> "EdgeConfig":
        """Build an :class:`EdgeConfig` from a parsed argparse :class:`~argparse.Namespace`.

        Layering order (highest precedence last):
        1. Dataclass defaults (constants defined above).
        2. JSON config file — ``args.config`` path (applied if present).
        3. Explicit CLI flags — any arg whose value is not ``None`` (the sentinel
           used by :func:`add_common_ingest_args` and :func:`add_anemometer_args`).

        Boolean toggle flags (``--fsync-every-row``, ``--raw-log``) use
        ``store_const=True`` / ``default=None``; ``None`` means "not given on
        CLI, let JSON config or code default apply."
        """
        cfg = cls()

        # --- 2. JSON config file overlay ---
        config_path: Path | None = getattr(args, "config", None)
        if config_path is not None:
            _apply_json_config(cfg, config_path)

        # --- 3. Explicit CLI args (non-None wins over JSON) ---
        if subcommand in ("receive", "web"):
            if getattr(args, "port", None) is not None:
                cfg.ingest.port = args.port
            if getattr(args, "baud", None) is not None:
                cfg.ingest.baud = args.baud
            if getattr(args, "data_dir", None) is not None:
                cfg.ingest.data_dir = args.data_dir
            if getattr(args, "nodes", None) is not None:
                cfg.ingest.nodes = args.nodes
            if getattr(args, "metrics_interval", None) is not None:
                cfg.ingest.metrics_interval_s = args.metrics_interval
            if getattr(args, "sync_interval", None) is not None:
                cfg.ingest.sync_interval_s = args.sync_interval
            # Boolean toggles: None = not given, True = explicitly set
            if getattr(args, "fsync_every_row", None):
                cfg.ingest.fsync_every_row = True
            if getattr(args, "raw_log", None):
                cfg.ingest.raw_log = True
            if getattr(args, "anemometer_port", None) is not None:
                cfg.ingest.anemometer.port = args.anemometer_port
            if getattr(args, "anemometer_baud", None) is not None:
                cfg.ingest.anemometer.baud = args.anemometer_baud
            if getattr(args, "anemometer_address", None) is not None:
                cfg.ingest.anemometer.address = args.anemometer_address
            if getattr(args, "anemometer_interval", None) is not None:
                cfg.ingest.anemometer.interval_s = args.anemometer_interval

        if subcommand == "web":
            # --host / --http-port are web-specific and always have real argparse
            # defaults, so we apply them unconditionally.
            cfg.web_host = args.host
            cfg.web_http_port = args.http_port
            if getattr(args, "sniffer_port", None) is not None:
                cfg.ingest.sniffer.port = args.sniffer_port
            if getattr(args, "sniffer_baud", None) is not None:
                cfg.ingest.sniffer.baud = args.sniffer_baud
            if getattr(args, "num_slots", None) is not None:
                cfg.ingest.sniffer.num_slots = args.num_slots

        if subcommand == "visualize":
            # visualize defines its own args with real defaults (not None),
            # so apply them unconditionally.
            cfg.ingest.port = args.port
            cfg.ingest.baud = args.baud
            cfg.ingest.sync_interval_s = args.sync_interval
            cfg.telemetry_rows = args.telemetry_rows

        return cfg


# ---------------------------------------------------------------------------
# JSON config file loader
# ---------------------------------------------------------------------------

def _apply_json_config(cfg: EdgeConfig, path: Path) -> None:
    """Overlay a JSON config file onto *cfg* in-place.

    Expected JSON schema (all keys optional)::

        {
          "ingest": {
            "port": "/dev/smartfires-base",
            "baud": 115200,
            "data_dir": "/mnt/nvme_drive/data",
            "nodes": [1, 2],
            "metrics_interval_s": 10,
            "sync_interval_s": 600,
            "fsync_every_row": false,
            "raw_log": false
          },
          "anemometer": {
            "port": null,
            "baud": 9600,
            "address": 1,
            "interval_s": 1.0
          },
          "web": {
            "host": "0.0.0.0",
            "http_port": 8080
          },
          "sniffer": {
            "port": null,
            "baud": 115200,
            "num_slots": 4
          }
        }

    Unknown keys are silently ignored.
    """
    with open(path, encoding="utf-8") as f:
        data: dict[str, Any] = json.load(f)

    ingest = data.get("ingest", {})
    if "port" in ingest:
        cfg.ingest.port = str(ingest["port"])
    if "baud" in ingest:
        cfg.ingest.baud = int(ingest["baud"])
    if "data_dir" in ingest:
        cfg.ingest.data_dir = Path(ingest["data_dir"])
    if "nodes" in ingest:
        cfg.ingest.nodes = [int(n) for n in ingest["nodes"]]
    if "metrics_interval_s" in ingest:
        cfg.ingest.metrics_interval_s = int(ingest["metrics_interval_s"])
    if "sync_interval_s" in ingest:
        cfg.ingest.sync_interval_s = int(ingest["sync_interval_s"])
    if "fsync_every_row" in ingest:
        cfg.ingest.fsync_every_row = bool(ingest["fsync_every_row"])
    if "raw_log" in ingest:
        cfg.ingest.raw_log = bool(ingest["raw_log"])

    anemometer = data.get("anemometer", {})
    if "port" in anemometer:
        cfg.ingest.anemometer.port = anemometer["port"] or None  # "" / null → disabled
    if "baud" in anemometer:
        cfg.ingest.anemometer.baud = int(anemometer["baud"])
    if "address" in anemometer:
        cfg.ingest.anemometer.address = int(anemometer["address"])
    if "interval_s" in anemometer:
        cfg.ingest.anemometer.interval_s = float(anemometer["interval_s"])

    web = data.get("web", {})
    if "host" in web:
        cfg.web_host = str(web["host"])
    if "http_port" in web:
        cfg.web_http_port = int(web["http_port"])

    sniffer = data.get("sniffer", {})
    if "port" in sniffer:
        cfg.ingest.sniffer.port = sniffer["port"] or None
    if "baud" in sniffer:
        cfg.ingest.sniffer.baud = int(sniffer["baud"])
    if "num_slots" in sniffer:
        cfg.ingest.sniffer.num_slots = int(sniffer["num_slots"])


# ---------------------------------------------------------------------------
# Shared argparse helpers
# ---------------------------------------------------------------------------

def add_common_ingest_args(parser: Any) -> None:
    """Register UART + ingest arguments on *parser*.

    Used by both ``receive`` and ``web`` subparsers to eliminate duplication.
    All defaults are ``None`` so :meth:`EdgeConfig.from_args` can distinguish
    "not provided on CLI" (``None``) from "explicitly set" when layering with
    the optional JSON config file.  The real defaults are shown in each
    ``--help`` string and live in the constants at the top of this module.
    """
    parser.add_argument(
        "--port",
        default=None,
        help=f"Serial port for the base-station USB link (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=None,
        help=f"Baud rate (default: {DEFAULT_BAUD})",
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=None,
        metavar="DIR",
        help=f"Root directory for telemetry CSV + metrics output (default: {DEFAULT_DATA_DIR})",
    )
    parser.add_argument(
        "--nodes",
        nargs="+",
        type=int,
        default=None,
        metavar="ID",
        help=f"Node IDs to track for packet-loss metrics (default: {DEFAULT_NODES})",
    )
    parser.add_argument(
        "--metrics-interval",
        type=int,
        default=None,
        metavar="SEC",
        help=f"Packet-loss state flush interval in seconds (default: {DEFAULT_METRICS_INTERVAL_S})",
    )
    parser.add_argument(
        "--sync-interval",
        type=int,
        default=None,
        metavar="SEC",
        help=f"Periodic TIME_SYNC broadcast interval in seconds (default: {DEFAULT_SYNC_INTERVAL_S})",
    )
    parser.add_argument(
        "--fsync-every-row",
        action="store_const",
        const=True,
        default=None,
        help="Call fsync() after every CSV row for maximum durability (default: off)",
    )
    parser.add_argument(
        "--raw-log",
        action="store_const",
        const=True,
        default=None,
        help="Write raw UART frames to a .jsonl log file (default: off)",
    )
    parser.add_argument(
        "--config",
        type=Path,
        default=None,
        metavar="PATH",
        help=(
            "Optional JSON config file.  Values from the file are overridden "
            "by any explicit CLI flag."
        ),
    )


def add_anemometer_args(parser: Any) -> None:
    """Register optional ES-W302 anemometer arguments on *parser*.

    Used by both ``receive`` and ``web`` subparsers.  As with
    :func:`add_common_ingest_args`, all defaults are ``None`` so the JSON
    config file layer in :meth:`EdgeConfig.from_args` can detect what the user
    explicitly set.
    """
    parser.add_argument(
        "--anemometer-port",
        default=None,
        metavar="PORT",
        help="Serial port for ES-W302 anemometer; omit to disable Modbus polling (default: disabled)",
    )
    parser.add_argument(
        "--anemometer-baud",
        type=int,
        default=None,
        metavar="BAUD",
        help=f"Anemometer baud rate (default: {DEFAULT_ANEMOMETER_BAUD})",
    )
    parser.add_argument(
        "--anemometer-address",
        type=int,
        default=None,
        metavar="ADDR",
        help=f"Anemometer Modbus device address (default: {DEFAULT_ANEMOMETER_ADDRESS})",
    )
    parser.add_argument(
        "--anemometer-interval",
        type=float,
        default=None,
        metavar="SEC",
        help=f"Anemometer Modbus poll interval in seconds (default: {DEFAULT_ANEMOMETER_INTERVAL_S})",
    )
