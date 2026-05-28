import math
import copy
import threading
import time
from pathlib import Path
from typing import Any

import numpy as np

from smartfires_edge.state_store import atomic_write_json, read_json


class SessionManager:
    def __init__(self, path: Path | None = None) -> None:
        self._path = path or (Path.home() / ".smartfires" / "session.json")
        self._lock = threading.Lock()
        self._state = self._default_state()
        self.load()

    def _default_state(self) -> dict[str, Any]:
        return {
            "node_id_to_uid_hash": {},
            "uid_hash_to_node_id": {},
            "calibrations": {},
            "command_queue": [],
            "node_status": {},
            "last_updated": int(time.time()),
        }

    @staticmethod
    def _uid_key(uid_hash: int) -> str:
        return f"0x{uid_hash:08X}"

    @staticmethod
    def _parse_uid_key(uid_key: str) -> int:
        if uid_key.startswith("0x") or uid_key.startswith("0X"):
            return int(uid_key, 16)
        return int(uid_key)

    def load(self) -> None:
        with self._lock:
            raw = read_json(self._path)
            if not raw:
                self._state = self._default_state()
                return

            node_map_raw = raw.get("node_id_to_uid_hash", {})
            uid_map_raw = raw.get("uid_hash_to_node_id", {})
            cal_raw = raw.get("calibrations", {})
            status_raw = raw.get("node_status", {})

            node_id_to_uid_hash = {
                int(node_id): self._parse_uid_key(str(uid_hash))
                for node_id, uid_hash in node_map_raw.items()
            }
            uid_hash_to_node_id = {
                self._parse_uid_key(str(uid_hash)): int(node_id)
                for uid_hash, node_id in uid_map_raw.items()
            }
            calibrations = {
                self._parse_uid_key(str(uid_hash)): value
                for uid_hash, value in cal_raw.items()
            }
            node_status = {
                int(node_id): value
                for node_id, value in status_raw.items()
            }

            self._state = {
                "node_id_to_uid_hash": node_id_to_uid_hash,
                "uid_hash_to_node_id": uid_hash_to_node_id,
                "calibrations": calibrations,
                "command_queue": raw.get("command_queue", []),
                "node_status": node_status,
                "last_updated": int(raw.get("last_updated", time.time())),
            }

    def save(self) -> None:
        with self._lock:
            self._save_locked()

    def _save_locked(self) -> None:
        node_map = {
            str(node_id): self._uid_key(uid_hash)
            for node_id, uid_hash in self._state["node_id_to_uid_hash"].items()
        }
        uid_map = {
            self._uid_key(uid_hash): node_id
            for uid_hash, node_id in self._state["uid_hash_to_node_id"].items()
        }
        calibrations = {
            self._uid_key(uid_hash): value
            for uid_hash, value in self._state["calibrations"].items()
        }
        node_status = {
            str(node_id): value
            for node_id, value in self._state["node_status"].items()
        }

        payload = {
            "node_id_to_uid_hash": node_map,
            "uid_hash_to_node_id": uid_map,
            "calibrations": calibrations,
            "command_queue": self._state["command_queue"],
            "node_status": node_status,
            "last_updated": int(time.time()),
        }
        atomic_write_json(self._path, payload)

    def get_uid_hash_for_node(self, node_id: int) -> int | None:
        with self._lock:
            return self._state["node_id_to_uid_hash"].get(int(node_id))

    def on_awaken(self, node_id: int, uid_hash: int) -> dict[str, Any]:
        with self._lock:
            node_id = int(node_id)
            uid_hash = int(uid_hash)
            self._state["node_id_to_uid_hash"][node_id] = uid_hash
            self._state["uid_hash_to_node_id"][uid_hash] = node_id
            node_status = self._state["node_status"].setdefault(node_id, {})
            node_status["last_seen"] = int(time.time())
            has_calibration = uid_hash in self._state["calibrations"]
            self._save_locked()
            return {
                "node_id": node_id,
                "uid_hash": uid_hash,
                "has_calibration": has_calibration,
            }

    def mark_node_seen(self, node_id: int) -> None:
        with self._lock:
            node_id = int(node_id)
            node_status = self._state["node_status"].setdefault(node_id, {})
            node_status["last_seen"] = int(time.time())

    def on_calibration_data(self, node_id: int, uid_hash: int, stats: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            node_id = int(node_id)
            uid_hash = int(uid_hash)
            if uid_hash != 0:
                self._state["node_id_to_uid_hash"][node_id] = uid_hash
                self._state["uid_hash_to_node_id"][uid_hash] = node_id

            sample_count = int(stats.get("sample_count", 0))
            mag_cov = stats.get("mag_cov", [])
            mag_mean = stats.get("mag_mean", [0.0, 0.0, 0.0])
            mag_min = stats.get("mag_min", [0.0, 0.0, 0.0])
            mag_max = stats.get("mag_max", [0.0, 0.0, 0.0])

            if len(mag_cov) != 6:
                return {"accepted": False, "reason": "invalid_covariance"}

            cov_xx, cov_yy, cov_zz, cov_xy, cov_xz, cov_yz = [float(v) for v in mag_cov]
            C = np.array(
                [
                    [cov_xx, cov_xy, cov_xz],
                    [cov_xy, cov_yy, cov_yz],
                    [cov_xz, cov_yz, cov_zz],
                ],
                dtype=float,
            )

            eigenvalues, V = np.linalg.eigh(C)
            eigen_ok = bool(np.all(eigenvalues > 0.0))
            axis_ranges = [float(mag_max[i]) - float(mag_min[i]) for i in range(3)]
            range_ok = all(r >= 20.0 for r in axis_ranges)
            sample_ok = sample_count >= 200
            hard_iron = np.array([float(v) for v in mag_mean], dtype=float)
            hard_iron_ok = float(np.linalg.norm(hard_iron)) < 200.0

            accepted = eigen_ok and range_ok and hard_iron_ok
            if not accepted:
                return {
                    "accepted": False,
                    "sample_ok": sample_ok,
                    "range_ok": range_ok,
                    "eigen_ok": eigen_ok,
                    "hard_iron_ok": hard_iron_ok,
                    "eigenvalues": [float(v) for v in eigenvalues],
                    "axis_ranges": axis_ranges,
                }

            scales = 1.0 / np.sqrt(np.maximum(eigenvalues, 1e-6))
            soft_iron = V @ np.diag(scales) @ V.T

            status = "valid" if sample_ok else "low_sample_count"
            # sensor_to_body is always reset to identity on new calibration data.
            # A fresh alignment fit (set_alignment) is required after every recalibration.
            self._state["calibrations"][uid_hash] = {
                "hard_iron": [float(v) for v in hard_iron.tolist()],
                "soft_iron": [[float(v) for v in row] for row in soft_iron.tolist()],
                "sensor_to_body": [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
                "sample_count": sample_count,
                "timestamp": int(time.time()),
                "status": status,
                "eigenvalues": [float(v) for v in eigenvalues],
                "axis_ranges": axis_ranges,
            }
            self._save_locked()
            return {
                "accepted": True,
                "status": status,
                "eigenvalues": [float(v) for v in eigenvalues],
                "axis_ranges": axis_ranges,
            }

    def on_cmd_ack(self, node_id: int, uid_hash: int, cmd_type: int, status: int) -> dict[str, Any]:
        with self._lock:
            node_id = int(node_id)
            uid_hash = int(uid_hash)
            if uid_hash != 0:
                self._state["node_id_to_uid_hash"][node_id] = uid_hash
                self._state["uid_hash_to_node_id"][uid_hash] = node_id

            entry = {
                "type": int(cmd_type),
                "node_id": node_id,
                "uid_hash": uid_hash,
                "sent_at": int(time.time()),
                "acked": True,
                "status": int(status),
            }
            self._state["command_queue"].append(entry)
            if len(self._state["command_queue"]) > 256:
                self._state["command_queue"] = self._state["command_queue"][-256:]
            self._save_locked()
            return entry

    @staticmethod
    def _magnetic_declination(lat: float | None, lon: float | None) -> float:
        if lat is None or lon is None:
            return 0.0
        return 0.0

    @classmethod
    def _compute_heading(cls, status: dict[str, Any], calibration: dict[str, Any]) -> dict[str, float] | None:
        try:
            mag_raw = np.array(
                [
                    float(status["mag_x"]),
                    float(status["mag_y"]),
                    float(status["mag_z"]),
                ],
                dtype=float,
            ) / 10.0
            accel_raw = np.array(
                [
                    float(status["accel_x"]),
                    float(status["accel_y"]),
                    float(status["accel_z"]),
                ],
                dtype=float,
            ) / 1000.0
        except (KeyError, TypeError, ValueError):
            return None

        hard_iron = np.array(calibration["hard_iron"], dtype=float)
        soft_iron = np.array(calibration["soft_iron"], dtype=float)

        # The SparkFun ICM-20948 library returns magnetometer data in the AK09916
        # sub-chip frame, which is rotated relative to the ICM-20948 accel/gyro frame.
        # Permute to the ICM-20948 body frame before applying calibration so that
        # mag and accel share the same coordinate system for tilt compensation.
        # AK09916-X → body-Y, AK09916-Y → body-X, AK09916-Z → -body-Z
        mag_icm = np.array([mag_raw[1], mag_raw[0], -mag_raw[2]], dtype=float)

        mag_c = soft_iron @ (mag_icm - hard_iron)

        # Rotate both mag and accel from the ICM-20948 board frame into the vehicle
        # body frame. Defaults to identity when no alignment has been fitted yet.
        R_sb_list = calibration.get("sensor_to_body")
        R_sb = np.array(R_sb_list, dtype=float) if R_sb_list is not None else np.eye(3)
        mag_body = R_sb @ mag_c
        accel_body = R_sb @ accel_raw

        roll = math.atan2(accel_body[1], accel_body[2])
        pitch = math.atan2(-accel_body[0], math.sqrt(accel_body[1] ** 2 + accel_body[2] ** 2))

        mx_h = mag_body[0] * math.cos(pitch) + mag_body[2] * math.sin(pitch)
        my_h = (
            mag_body[0] * math.sin(roll) * math.sin(pitch)
            + mag_body[1] * math.cos(roll)
            - mag_body[2] * math.sin(roll) * math.cos(pitch)
        )

        heading_mag = math.degrees(math.atan2(-my_h, mx_h)) % 360.0
        lat = status.get("lat") if status.get("gps_valid") else None
        lon = status.get("lon") if status.get("gps_valid") else None
        declination = cls._magnetic_declination(lat, lon)
        heading_true = (heading_mag + declination) % 360.0

        return {
            "heading_true_deg": round(heading_true, 1),
            "pitch_deg": round(math.degrees(pitch), 1),
            "roll_deg": round(math.degrees(roll), 1),
        }

    def on_status(self, node_id: int, uid_hash: int | None, status: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            node_id = int(node_id)
            if uid_hash is None:
                uid_hash = self._state["node_id_to_uid_hash"].get(node_id)
            elif uid_hash != 0:
                uid_hash = int(uid_hash)
                self._state["node_id_to_uid_hash"][node_id] = uid_hash
                self._state["uid_hash_to_node_id"][uid_hash] = node_id

            node_status = self._state["node_status"].setdefault(node_id, {})
            node_status["last_seen"] = int(time.time())

            if not status.get("imu_valid") or uid_hash is None:
                return {"computed": False}

            calibration = self._state["calibrations"].get(uid_hash)
            if not calibration:
                return {"computed": False}

            heading = self._compute_heading(status, calibration)
            if not heading:
                return {"computed": False}

            node_status.update(heading)
            node_status["last_heading_ts"] = int(time.time())
            return {"computed": True, **heading}

    @staticmethod
    def fit_sensor_to_body(
        observations: list[tuple[list[float], list[float]]],
    ) -> tuple[np.ndarray, float]:
        """Fit a rotation matrix R_sb that maps the ICM-20948 board frame into the
        vehicle body frame using the Wahba/Kabsch method.

        observations: list of (sensor_vec, body_vec) pairs from static poses.
            sensor_vec — averaged mag_body (post-AK09916 permutation, post-soft-iron)
                         or averaged accel_raw, in the ICM-20948 board frame.
            body_vec   — known reference direction in the vehicle body frame
                         (e.g. [1,0,0] when the nose points forward during that pose).
        Vectors need not be unit-length; they are normalised internally.
        At least 3 non-coplanar pose pairs are required.

        Returns (R_sb, rms_residual_deg). Raise if fewer than 3 observations supplied.
        Reject the result if rms_residual_deg > 5.0 — see plan validation criteria.
        """
        if len(observations) < 3:
            raise ValueError("at least 3 pose observations are required to fit R_sb")

        H = np.zeros((3, 3), dtype=float)
        for s_vec, b_vec in observations:
            s = np.asarray(s_vec, dtype=float)
            b = np.asarray(b_vec, dtype=float)
            s_norm = np.linalg.norm(s)
            b_norm = np.linalg.norm(b)
            if s_norm < 1e-9 or b_norm < 1e-9:
                raise ValueError("zero-length vector in observations")
            H += np.outer(b / b_norm, s / s_norm)

        U, _, Vt = np.linalg.svd(H)
        # Enforce det = +1 to avoid reflections
        d = float(np.linalg.det(U @ Vt))
        R_sb = U @ np.diag([1.0, 1.0, d]) @ Vt

        errors: list[float] = []
        for s_vec, b_vec in observations:
            s = np.asarray(s_vec, dtype=float)
            b = np.asarray(b_vec, dtype=float)
            s /= np.linalg.norm(s)
            b /= np.linalg.norm(b)
            b_pred = R_sb @ s
            cos_err = float(np.clip(np.dot(b_pred, b), -1.0, 1.0))
            errors.append(math.degrees(math.acos(cos_err)))

        rms_deg = math.sqrt(sum(e * e for e in errors) / len(errors))
        return R_sb, rms_deg

    def set_alignment(self, node_id: int, R_sb: np.ndarray) -> dict[str, Any]:
        """Persist a fitted sensor-to-body rotation matrix for the given node.

        R_sb must be the output of fit_sensor_to_body. Caller is responsible for
        rejecting fits with rms_residual_deg > 5.0 before calling this method.
        Returns {"stored": True} on success or {"stored": False, "reason": str}.
        """
        with self._lock:
            node_id = int(node_id)
            uid_hash = self._state["node_id_to_uid_hash"].get(node_id)
            if uid_hash is None or uid_hash not in self._state["calibrations"]:
                return {"stored": False, "reason": "no_calibration_for_node"}
            self._state["calibrations"][uid_hash]["sensor_to_body"] = [
                [float(v) for v in row] for row in R_sb.tolist()
            ]
            self._save_locked()
            return {"stored": True}

    def clear_calibration_by_node(self, node_id: int) -> bool:
        with self._lock:
            node_id = int(node_id)
            uid_hash = self._state["node_id_to_uid_hash"].get(node_id)
            if uid_hash is None or uid_hash not in self._state["calibrations"]:
                return False
            del self._state["calibrations"][uid_hash]
            self._save_locked()
            return True

    def clear_calibrations(self) -> None:
        with self._lock:
            self._state["calibrations"] = {}
            self._save_locked()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._state)
