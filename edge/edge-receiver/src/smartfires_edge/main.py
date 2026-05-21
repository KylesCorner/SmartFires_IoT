import argparse
from pathlib import Path

from smartfires_edge.cli import run_cli
from smartfires_edge.ingest_service import run_receive
from smartfires_edge.packet_loss import print_summary
from smartfires_edge.visualize_service import run_visualize


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="smartfires-edge")
    sub = p.add_subparsers(dest="command", required=True)

    recv = sub.add_parser("receive", help="Run UART ingest and durable CSV logging")
    recv.add_argument("--port", default="/dev/ttyTHS1")
    recv.add_argument("--baud", type=int, default=115200)
    recv.add_argument("--data-dir", type=Path, default=Path("/mnt/nvme_drive/data"))
    recv.add_argument("--nodes", nargs="+", type=int, default=[1, 2])
    recv.add_argument("--metrics-interval", type=int, default=10)
    recv.add_argument("--sync-interval", type=int, default=600)
    recv.add_argument("--ack-interval", type=float, default=4.0)
    recv.add_argument("--fsync-every-row", action="store_true")
    recv.add_argument("--raw-log", action="store_true")
    recv.add_argument("--anemometer-port", default=None)
    recv.add_argument("--anemometer-baud", type=int, default=9600)
    recv.add_argument("--anemometer-address", type=int, default=1)
    recv.add_argument("--anemometer-interval", type=float, default=1.0)

    summary = sub.add_parser("summary", help="Print current packet-loss summary")
    summary.add_argument("--data-dir", type=Path, default=Path("/mnt/nvme_drive/data"))

    visualize = sub.add_parser("visualize", help="Render live telemetry/status tables")
    visualize.add_argument("--port", default="/dev/ttyTHS1")
    visualize.add_argument("--baud", type=int, default=115200)
    visualize.add_argument("--sync-interval", type=int, default=600)
    visualize.add_argument("--ack-interval", type=float, default=4.0)
    visualize.add_argument("--telemetry-rows", type=int, default=20)

    cli = sub.add_parser("cli", help="Interactive Jetson CLI")
    cli.add_argument("--port", default="/dev/ttyTHS1")
    cli.add_argument("--baud", type=int, default=115200)
    cli.add_argument("--session-file", type=Path, default=None)

    return p


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.command == "receive":
        return run_receive(
            port=args.port,
            baud=args.baud,
            data_dir=args.data_dir,
            nodes=args.nodes,
            metrics_interval_s=args.metrics_interval,
            sync_interval_s=args.sync_interval,
            ack_interval_s=args.ack_interval,
            fsync_every_row=args.fsync_every_row,
            raw_log=args.raw_log,
            anemometer_port=args.anemometer_port,
            anemometer_baud=args.anemometer_baud,
            anemometer_address=args.anemometer_address,
            anemometer_interval_s=args.anemometer_interval,
        )

    if args.command == "summary":
        print_summary(args.data_dir)
        return 0

    if args.command == "visualize":
        return run_visualize(
            port=args.port,
            baud=args.baud,
            sync_interval_s=args.sync_interval,
            ack_interval_s=args.ack_interval,
            telemetry_rows_max=max(1, int(args.telemetry_rows)),
        )

    if args.command == "cli":
        return run_cli(
            port=args.port,
            baud=args.baud,
            session_file=args.session_file,
        )

    parser.error("Unknown command")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
