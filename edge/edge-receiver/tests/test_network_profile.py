import json
import struct
import tempfile
import unittest
from pathlib import Path

from smartfires_edge.packet import (
    FRAME_M0,
    FRAME_M1,
    HEADER_FMT,
    NETWORK_PROFILE_PAYLOAD_FMT,
    PKT_MAGIC,
    PKT_NETWORK_PROFILE,
    crc8,
    decode_network_profile,
)
from smartfires_edge.session_meta import SessionMetaLogger
from smartfires_edge.sniffer_service import _slot_timing
from smartfires_edge.uart_receiver import FrameReceiver
from smartfires_edge.live_state import LiveState
from smartfires_edge.web.app import create_app


def _profile(sf=12, fingerprint="1234abcd"):
    return {
        "profile_id": f"sf{sf}",
        "spreading_factor": sf,
        "bandwidth_hz": 250_000 if sf == 12 else 125_000,
        "coding_rate": "4/5",
        "coding_rate_denominator": 5,
        "num_slots": 5,
        "slot_width_ms": 5_000 if sf == 12 else 900,
        "guard_ms": 100 if sf == 12 else 20,
        "max_bundle_deltas": 7 if sf == 12 else 14,
        "continuous_sample_period_ms": 4_000 if sf == 12 else 750,
        "timed_sample_period_ms": 4_000 if sf == 12 else 1_000,
        "status_interval_ms": 300_000 if sf == 12 else 15_000,
        "fingerprint": fingerprint,
    }


def _profile_raw(sf=12, fingerprint=0x1234ABCD):
    header = struct.pack(HEADER_FMT, PKT_MAGIC, PKT_NETWORK_PROFILE, 1, 9, 0)
    profile = _profile(sf, f"{fingerprint:08x}")
    payload = struct.pack(
        NETWORK_PROFILE_PAYLOAD_FMT,
        1,
        sf,
        sf,
        profile["coding_rate_denominator"],
        profile["bandwidth_hz"],
        profile["num_slots"],
        profile["slot_width_ms"],
        profile["guard_ms"],
        profile["max_bundle_deltas"],
        profile["continuous_sample_period_ms"],
        profile["timed_sample_period_ms"],
        profile["status_interval_ms"],
        fingerprint,
    )
    return header + payload


class NetworkProfileTests(unittest.TestCase):
    def test_binary_profile_decodes_through_existing_outer_frame(self):
        raw = _profile_raw()
        decoded = decode_network_profile(raw)
        self.assertEqual(decoded, _profile())

        data = bytes([0]) + raw  # base frame's signed RSSI/control byte
        frame = bytes([FRAME_M0, FRAME_M1, len(data)]) + data
        frame += bytes([crc8(frame[2:])])
        receiver = FrameReceiver(0.0)
        event = None
        for byte in frame:
            candidate = receiver.push_byte(byte)
            if candidate is not None:
                event = candidate
        self.assertIsNotNone(event)
        self.assertEqual(event["pkt_type"], PKT_NETWORK_PROFILE)
        self.assertEqual(event["network_profile"], _profile())

    def test_decoder_rejects_unknown_schema_and_inconsistent_sf(self):
        bad_schema = bytearray(_profile_raw())
        bad_schema[struct.calcsize(HEADER_FMT)] = 2
        self.assertIsNone(decode_network_profile(bytes(bad_schema)))

        inconsistent = bytearray(_profile_raw())
        inconsistent[struct.calcsize(HEADER_FMT) + 2] = 10
        self.assertIsNone(decode_network_profile(bytes(inconsistent)))

        wrong_sf12_bandwidth = bytearray(_profile_raw())
        bandwidth_offset = struct.calcsize(HEADER_FMT) + struct.calcsize("<BBBB")
        struct.pack_into("<I", wrong_sf12_bandwidth, bandwidth_offset, 125_000)
        self.assertIsNone(decode_network_profile(bytes(wrong_sf12_bandwidth)))

        wrong_sf10_bandwidth = bytearray(_profile_raw(sf=10))
        struct.pack_into("<I", wrong_sf10_bandwidth, bandwidth_offset, 250_000)
        self.assertIsNone(decode_network_profile(bytes(wrong_sf10_bandwidth)))

    def test_session_starts_unknown_then_pins_profile_after_telemetry(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            meta = SessionMetaLogger(1, 1_800_000_000.0, "fake", 115200, root)
            initial = json.loads((root / "session.json").read_text())
            self.assertEqual(initial["network_profile"]["state"], "unknown")
            self.assertIsNone(initial["network_profile"]["active"])

            first = meta.on_network_profile(_profile())
            self.assertTrue(first["accepted"])
            self.assertEqual(first["snapshot"]["source"], "base")
            meta.mark_telemetry_recorded()

            changed_profile = _profile(10, "89abcdef")
            changed = meta.on_network_profile(changed_profile)
            self.assertFalse(changed["accepted"])
            self.assertFalse(meta.telemetry_allowed())
            self.assertEqual(changed["snapshot"]["state"], "mismatch")
            self.assertEqual(changed["snapshot"]["active"]["profile_id"], "sf12")
            self.assertEqual(changed["snapshot"]["base"]["profile_id"], "sf10")
            self.assertIn("fingerprint", changed["snapshot"]["mismatches"])

            # A repeated periodic announcement must remain rejected rather than
            # silently becoming the session identity on its second appearance.
            repeated = meta.on_network_profile(changed_profile)
            self.assertFalse(repeated["accepted"])
            self.assertEqual(repeated["snapshot"]["active"]["profile_id"], "sf12")
            self.assertEqual(len(repeated["snapshot"]["history"]), 1)
            self.assertFalse((root / "session.json.tmp").exists())

            restored = meta.on_network_profile(_profile())
            self.assertTrue(restored["accepted"])
            self.assertTrue(meta.telemetry_allowed())

    def test_override_is_active_and_disagreement_is_visible(self):
        with tempfile.TemporaryDirectory() as td:
            meta = SessionMetaLogger(
                1,
                1_800_000_000.0,
                "fake",
                115200,
                Path(td),
                network_profile_override=_profile(12, "1234abcd"),
            )
            result = meta.on_network_profile(_profile(10, "89abcdef"))["snapshot"]
            self.assertEqual(result["state"], "mismatch")
            self.assertEqual(result["source"], "override")
            self.assertEqual(result["active"]["profile_id"], "sf12")
            self.assertEqual(result["base"]["profile_id"], "sf10")

    def test_api_exposes_unknown_profile_state(self):
        with tempfile.TemporaryDirectory() as td:
            app = create_app(LiveState([2]), Path(td), tile_cache_dir=Path(td) / "tiles")
            endpoint = next(
                route.endpoint
                for route in app.routes
                if getattr(route, "path", None) == "/api/network_profile"
            )
            self.assertEqual(
                endpoint(),
                {
                    "state": "unknown",
                    "source": None,
                    "active": None,
                    "base": None,
                    "override": None,
                    "mismatches": [],
                    "history": [],
                },
            )

    def test_sniffer_uses_profile_geometry_and_reports_fingerprint_mismatch(self):
        jitter, violation = _slot_timing(
            node_id=2,
            session_ms=7_500,
            num_slots=5,
            slot_width_ms=5_000,
            guard_ms=100,
        )
        self.assertEqual(jitter, 0)
        self.assertFalse(violation)
        _, violation = _slot_timing(2, 5_000, 5, 5_000, 100)
        self.assertTrue(violation)

        live = LiveState([2])
        profile = _profile()
        live.set_network_profile_snapshot(
            {
                "state": "active",
                "source": "base",
                "active": profile,
                "base": profile,
                "override": None,
                "mismatches": [],
                "history": [],
            }
        )
        live.set_sniffer_network_profile(
            {"profile_id": "sf10", "fingerprint": "89abcdef"}
        )
        snapshot = live.network_profile_snapshot()
        self.assertEqual(snapshot["state"], "mismatch")
        self.assertIn("sniffer.fingerprint", snapshot["mismatches"])


if __name__ == "__main__":
    unittest.main()
