"""Validation and state helpers for base-announced network profiles."""

from __future__ import annotations

from typing import Any


PROFILE_FIELDS = (
    "profile_id",
    "spreading_factor",
    "bandwidth_hz",
    "coding_rate_denominator",
    "num_slots",
    "slot_width_ms",
    "guard_ms",
    "max_bundle_deltas",
    "continuous_sample_period_ms",
    "timed_sample_period_ms",
    "status_interval_ms",
    "fingerprint",
)


def normalize_network_profile(profile: dict[str, Any]) -> dict[str, Any]:
    """Return the canonical API/session representation or raise ``ValueError``.

    JSON recovery overrides accept ``sf`` as an alias for
    ``spreading_factor`` and either an integer selector or ``"sf12"`` for
    ``profile_id``.  Announcements produced by the binary decoder are already
    canonical and pass through this same validation boundary.
    """
    raw = dict(profile)
    sf = int(raw.get("spreading_factor", raw.get("sf", 0)))
    raw_id = raw.get("profile_id", sf)
    if isinstance(raw_id, str):
        selector_text = raw_id.lower().removeprefix("sf")
        selector = int(selector_text)
    else:
        selector = int(raw_id)
    if selector not in (7, 9, 10, 12) or sf != selector:
        raise ValueError("profile_id and spreading_factor must match one of 7, 9, 10, 12")

    denominator = raw.get("coding_rate_denominator")
    if denominator is None:
        coding_rate = str(raw.get("coding_rate", ""))
        try:
            numerator_text, denominator_text = coding_rate.split("/", 1)
            if int(numerator_text) != 4:
                raise ValueError
            denominator = int(denominator_text)
        except (TypeError, ValueError) as exc:
            raise ValueError("coding rate must be 4/N") from exc
    denominator = int(denominator)

    fingerprint_raw = raw.get("fingerprint")
    if isinstance(fingerprint_raw, int):
        fingerprint = f"{fingerprint_raw & 0xFFFFFFFF:08x}"
    elif isinstance(fingerprint_raw, str):
        fingerprint = fingerprint_raw.lower().removeprefix("0x")
        if len(fingerprint) > 8:
            raise ValueError("fingerprint must fit uint32")
        try:
            fingerprint = f"{int(fingerprint, 16):08x}"
        except ValueError as exc:
            raise ValueError("fingerprint must be hexadecimal") from exc
    else:
        raise ValueError("fingerprint is required")

    result = {
        "profile_id": f"sf{selector}",
        "spreading_factor": sf,
        "bandwidth_hz": int(raw.get("bandwidth_hz", 0)),
        "coding_rate": f"4/{denominator}",
        "coding_rate_denominator": denominator,
        "num_slots": int(raw.get("num_slots", 0)),
        "slot_width_ms": int(raw.get("slot_width_ms", 0)),
        "guard_ms": int(raw.get("guard_ms", 0)),
        "max_bundle_deltas": int(raw.get("max_bundle_deltas", 0)),
        "continuous_sample_period_ms": int(raw.get("continuous_sample_period_ms", 0)),
        "timed_sample_period_ms": int(raw.get("timed_sample_period_ms", 0)),
        "status_interval_ms": int(raw.get("status_interval_ms", 0)),
        "fingerprint": fingerprint,
    }
    expected_bandwidth_hz = 250_000 if selector == 12 else 125_000
    if (
        result["bandwidth_hz"] != expected_bandwidth_hz
        or denominator != 5
        or result["num_slots"] != 5
        or result["guard_ms"] <= 0
        or result["slot_width_ms"] <= 2 * result["guard_ms"]
        or not 0 < result["max_bundle_deltas"] <= 14
        or result["continuous_sample_period_ms"] <= 0
        or result["timed_sample_period_ms"] <= 0
        or result["status_interval_ms"] <= 0
    ):
        raise ValueError("network profile contains invalid timing or geometry")
    return result


def mismatch_fields(left: dict[str, Any] | None, right: dict[str, Any] | None) -> list[str]:
    if left is None or right is None:
        return []
    return [field for field in PROFILE_FIELDS if left.get(field) != right.get(field)]


def unknown_profile_state() -> dict[str, Any]:
    return {
        "state": "unknown",
        "source": None,
        "active": None,
        "base": None,
        "override": None,
        "mismatches": [],
        "history": [],
    }
