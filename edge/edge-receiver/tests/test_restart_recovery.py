import asyncio
import csv
import tempfile
import threading
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

from fastapi import BackgroundTasks

from smartfires_edge.base_station_store import BaseStationStore
from smartfires_edge.config import EdgeConfig, IngestConfig
from smartfires_edge.csv_logger import CSV_COLUMNS
from smartfires_edge.ingest_service import _create_session_dir, run_receive
from smartfires_edge.live_state import LiveState
from smartfires_edge.telemetry_cache import SessionTelemetryCache, _ts_ms
from smartfires_edge.uart_receiver import iter_packets
from smartfires_edge.web.app import create_app
from smartfires_edge.web_service import run_web


class _FakeSessionManager:
    def get_uid_hash_for_node(self, _node_id):
        return None


class _SilentSerial:
    def __init__(self):
        self.writes = []
        self.input_reset = False
        self.closed = False

    def reset_input_buffer(self):
        self.input_reset = True

    def read(self, _size):
        time.sleep(0.005)
        return b""

    def write(self, payload):
        self.writes.append(bytes(payload))
        return len(payload)

    def close(self):
        self.closed = True


class RestartRecoveryTests(unittest.TestCase):
    def test_session_directories_never_reuse_second_resolution_name(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            started = 1_800_000_000.25
            first = _create_session_dir(root, started, 0x11111111)
            second = _create_session_dir(root, started, 0x22222222)
            self.assertNotEqual(first, second)
            self.assertTrue(first.is_dir())
            self.assertTrue(second.is_dir())
            self.assertIn("22222222", second.name)

    def test_silent_serial_open_bootstraps_before_any_inbound_frame(self):
        serial_obj = _SilentSerial()
        opened = threading.Event()
        stop = threading.Event()
        result = []

        def silent_packets(_port, _baud, session_start, stop_event, on_open):
            self.assertIsNotNone(session_start)
            on_open(serial_obj)
            opened.set()
            stop_event.wait(2)
            if False:
                yield None

        with tempfile.TemporaryDirectory() as td, mock.patch(
            "smartfires_edge.ingest_service.iter_packets", silent_packets
        ), mock.patch(
            "smartfires_edge.ingest_service.SessionManager", _FakeSessionManager
        ):
            cfg = IngestConfig(
                port="fake-base", data_dir=Path(td), nodes=[2], sync_interval_s=3600
            )
            worker = threading.Thread(
                target=lambda: result.append(run_receive(cfg, stop_event=stop)),
                daemon=True,
            )
            worker.start()
            self.assertTrue(opened.wait(2), "serial bootstrap did not finish")
            self.assertEqual(len(serial_obj.writes), 2, "expected reset then TIME_SYNC")
            stop.set()
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertEqual(result, [0])

    def test_uart_iterator_stops_while_port_is_silent(self):
        serial_obj = _SilentSerial()
        opened = threading.Event()
        stop = threading.Event()

        def consume():
            list(iter_packets("fake", 115200, stop_event=stop, on_open=lambda _s: opened.set()))

        with mock.patch(
            "smartfires_edge.uart_receiver.serial.Serial", return_value=serial_obj
        ):
            worker = threading.Thread(target=consume, daemon=True)
            worker.start()
            self.assertTrue(opened.wait(1))
            stop.set()
            worker.join(1)

        self.assertFalse(worker.is_alive())
        self.assertTrue(serial_obj.input_reset)
        self.assertTrue(serial_obj.closed)

    def test_new_session_ack_precedes_shutdown_signal_and_coalesces(self):
        with tempfile.TemporaryDirectory() as td:
            stop = threading.Event()
            app = create_app(
                LiveState([2]),
                Path(td),
                base_station_store=BaseStationStore(Path(td) / "base.json"),
                reset_event=stop,
                tile_cache_dir=Path(td) / "tiles",
            )
            endpoint = next(
                route.endpoint for route in app.routes
                if getattr(route, "path", None) == "/api/new_session"
            )

            tasks = BackgroundTasks()
            self.assertEqual(endpoint(tasks), {"status": "restart_requested"})
            self.assertFalse(stop.is_set(), "shutdown was signaled before response completion")
            asyncio.run(tasks())
            self.assertTrue(stop.is_set())

            duplicate_tasks = BackgroundTasks()
            self.assertEqual(
                endpoint(duplicate_tasks), {"status": "restart_already_requested"}
            )

    def test_required_ingest_exit_takes_down_web_process(self):
        class FakeServer:
            def __init__(self, _config):
                self.should_exit = False

            def run(self):
                deadline = time.monotonic() + 2
                while not self.should_exit and time.monotonic() < deadline:
                    time.sleep(0.01)

        with tempfile.TemporaryDirectory() as td, mock.patch(
            "smartfires_edge.web_service.run_receive", return_value=1
        ), mock.patch(
            "smartfires_edge.web_service.create_app", return_value=object()
        ), mock.patch(
            "smartfires_edge.web_service.uvicorn.Config", return_value=object()
        ), mock.patch(
            "smartfires_edge.web_service.uvicorn.Server", FakeServer
        ):
            cfg = EdgeConfig(ingest=IngestConfig(data_dir=Path(td)))
            self.assertEqual(run_web(cfg), 1)

    def test_link_connection_and_readiness_are_distinct(self):
        state = LiveState([2])
        state.set_link_connected(True)
        self.assertEqual(
            state.link_status() | {"changed_at": None},
            {"connected": True, "ready": False, "error": None, "changed_at": None},
        )
        state.set_link_ready(True)
        self.assertTrue(state.link_status()["ready"])
        state.set_link_connected(False, "gone")
        self.assertFalse(state.link_status()["ready"])

    def test_legacy_naive_utc_and_offset_timestamps_keep_their_instants(self):
        expected = int(datetime(2026, 11, 1, 6, 30, tzinfo=timezone.utc).timestamp() * 1000)
        self.assertEqual(_ts_ms("2026-11-01T06:30:00.000"), expected)
        self.assertEqual(_ts_ms("2026-11-01T01:30:00.000-05:00"), expected)

    def test_seventy_two_hour_history_crossing_year_remains_ordered(self):
        with tempfile.TemporaryDirectory() as td:
            csv_path = Path(td) / "telemetry.csv"
            start = datetime(2026, 12, 30, tzinfo=timezone.utc)
            with csv_path.open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=CSV_COLUMNS)
                writer.writeheader()
                for hour in range(73):
                    instant = start + timedelta(hours=hour)
                    writer.writerow({
                        "timestamp": instant.isoformat(timespec="milliseconds"),
                        "packet_type": "telemetry",
                        "node_id": 2,
                        "temp_c": 20 + hour / 10,
                    })

            cache = SessionTelemetryCache()
            result = cache.history(
                csv_path,
                node_id=2,
                metric="temp_c",
                start_ms=int(start.timestamp() * 1000),
                end_ms=int((start + timedelta(hours=72)).timestamp() * 1000),
                max_points=1000,
            )
            timestamps = [point[0] for point in result["points"]]
            self.assertEqual(len(timestamps), 73)
            self.assertEqual(timestamps, sorted(timestamps))
            self.assertEqual(
                timestamps[-1], int(datetime(2027, 1, 2, tzinfo=timezone.utc).timestamp() * 1000)
            )


if __name__ == "__main__":
    unittest.main()
