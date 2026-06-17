import argparse
from pathlib import Path

from smartfires_edge.config import (
    DEFAULT_BAUD,
    DEFAULT_PORT,
    DEFAULT_SYNC_INTERVAL_S,
    DEFAULT_TELEMETRY_ROWS,
    DEFAULT_WEB_HOST,
    DEFAULT_WEB_HTTP_PORT,
    EdgeConfig,
    add_anemometer_args,
    add_common_ingest_args,
)
from smartfires_edge.ingest_service import run_receive
from smartfires_edge.packet_loss import print_summary
from smartfires_edge.visualize_service import run_visualize
from smartfires_edge.web_service import run_web


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="smartfires-edge")
    sub = p.add_subparsers(dest="command", required=True)

    # ------------------------------------------------------------------
    # receive — UART ingest + durable CSV logging
    # ------------------------------------------------------------------
    recv = sub.add_parser("receive", help="Run UART ingest and durable CSV logging")
    add_common_ingest_args(recv)
    add_anemometer_args(recv)

    # ------------------------------------------------------------------
    # summary — print current packet-loss summary from saved state
    # ------------------------------------------------------------------
    summary = sub.add_parser("summary", help="Print current packet-loss summary")
    summary.add_argument("--data-dir", type=Path, default=Path("/mnt/nvme_drive/data"))

    # ------------------------------------------------------------------
    # visualize — live terminal telemetry/status tables
    # ------------------------------------------------------------------
    visualize = sub.add_parser("visualize", help="Render live telemetry/status tables")
    visualize.add_argument("--port", default=DEFAULT_PORT)
    visualize.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    visualize.add_argument(
        "--sync-interval",
        type=int,
        default=DEFAULT_SYNC_INTERVAL_S,
        metavar="SEC",
    )
    visualize.add_argument("--telemetry-rows", type=int, default=DEFAULT_TELEMETRY_ROWS)

    # ------------------------------------------------------------------
    # web — UART ingest + live FastAPI/uvicorn web dashboard
    # ------------------------------------------------------------------
    web = sub.add_parser("web", help="Run UART ingest + live web dashboard")
    add_common_ingest_args(web)
    add_anemometer_args(web)
    web.add_argument("--host", default=DEFAULT_WEB_HOST)
    web.add_argument("--http-port", type=int, default=DEFAULT_WEB_HTTP_PORT)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "receive":
        cfg = EdgeConfig.from_args(args, subcommand="receive")
        return run_receive(cfg.ingest)

    if args.command == "summary":
        print_summary(args.data_dir)
        return 0

    if args.command == "visualize":
        cfg = EdgeConfig.from_args(args, subcommand="visualize")
        return run_visualize(cfg)

    if args.command == "web":
        cfg = EdgeConfig.from_args(args, subcommand="web")
        return run_web(cfg)

    parser.error("Unknown command")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
