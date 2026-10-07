from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from smartfires_edge.network_profile import (
    mismatch_fields,
    normalize_network_profile,
    unknown_profile_state,
)
from smartfires_edge.state_store import atomic_write_json


class SessionMetaLogger:
    """Writes and atomically updates a session manifest alongside the daily telemetry CSV.

    Output: data_dir/telemetry/session-YYYY-MM-DD.json

    The date in the filename matches the telemetry CSV for the same session, so a
    post-processing script can find the metadata for any CSV by substituting
    'telemetry-' → 'session-'.
    """

    def __init__(
        self,
        session_id: int,
        session_start: float,
        port: str,
        baud: int,
        data_dir: Path,
        network_profile_override: dict[str, Any] | None = None,
    ) -> None:
        self._session_id = session_id
        self._session_start = session_start
        self._port = port
        self._baud = baud
        self._node_registry: dict[str, dict] = {}
        self._telemetry_recorded = False
        self._telemetry_allowed = True
        self._network_profile = unknown_profile_state()
        if network_profile_override is not None:
            override = self._stamp_profile(network_profile_override, "override")
            self._network_profile.update(
                state="active", source="override", active=override, override=override
            )
        self._path = data_dir / "session.json"
        self._write()

    @staticmethod
    def _stamp_profile(profile: dict[str, Any], source: str) -> dict[str, Any]:
        result = normalize_network_profile(profile)
        result["source"] = source
        result["first_observed_at"] = datetime.now(timezone.utc).isoformat(
            timespec="milliseconds"
        )
        return result

    def _write(self) -> None:
        payload = {
            "session_id": f"0x{self._session_id:08X}",
            "started_at": datetime.fromtimestamp(
                self._session_start, tz=timezone.utc
            ).isoformat(timespec="milliseconds"),
            "port": self._port,
            "baud": self._baud,
            "node_registry": self._node_registry,
            "network_profile": self._network_profile,
        }
        atomic_write_json(self._path, payload)

    def network_profile_snapshot(self) -> dict[str, Any]:
        """Return a detached copy suitable for LiveState/API publication."""
        import copy

        return copy.deepcopy(self._network_profile)

    def mark_telemetry_recorded(self) -> None:
        self._telemetry_recorded = True

    def telemetry_allowed(self) -> bool:
        return self._telemetry_allowed

    def on_network_profile(self, profile: dict[str, Any]) -> dict[str, Any]:
        """Apply a base announcement and atomically persist the resulting state.

        Once samples exist under a known base profile, a changed announcement
        becomes an explicit mismatch/history boundary.  The session's active
        identity stays pinned to the original profile.
        """
        incoming = self._stamp_profile(profile, "base")
        previous_base = self._network_profile.get("base")
        active = self._network_profile.get("active")
        override = self._network_profile.get("override")

        if previous_base is not None and not mismatch_fields(previous_base, incoming):
            incoming["first_observed_at"] = previous_base["first_observed_at"]
        elif active is not None and not mismatch_fields(active, incoming):
            # The base may return to the session's pinned profile after a
            # rejected transition; retain the original observation instant.
            incoming["first_observed_at"] = active["first_observed_at"]

        # ``active`` is the pinned session identity.  Comparing against it (not
        # merely the most recently observed base value) ensures repeated copies
        # of a rejected transition cannot become accepted on the next periodic
        # announcement.
        changed_after_data = (
            active is not None
            and active.get("source") == "base"
            and bool(mismatch_fields(active, incoming))
            and self._telemetry_recorded
        )

        if changed_after_data:
            # Keep the original active identity while publishing the newly seen
            # base profile as the mismatch counterpart.
            self._network_profile["base"] = incoming
            compare_to = override or active
            mismatches = mismatch_fields(compare_to, incoming)
            self._network_profile.update(
                state="mismatch",
                source="override" if override is not None else "base",
                mismatches=mismatches,
            )
            history = self._network_profile["history"]
            transition = {
                "observed_at": incoming["first_observed_at"],
                "from_fingerprint": active["fingerprint"],
                "to_fingerprint": incoming["fingerprint"],
                "after_telemetry": True,
                "accepted": False,
            }
            if not history or history[-1].get("to_fingerprint") != incoming["fingerprint"]:
                history.append(transition)
            accepted = False
            self._telemetry_allowed = False
        else:
            self._network_profile["base"] = incoming
            if override is not None:
                mismatches = mismatch_fields(override, incoming)
                self._network_profile.update(
                    state="mismatch" if mismatches else "active",
                    source="override",
                    active=override,
                    mismatches=mismatches,
                )
            else:
                self._network_profile.update(
                    state="active",
                    source="base",
                    active=incoming,
                    mismatches=[],
                )
            accepted = True
            self._telemetry_allowed = True

        self._write()
        return {"accepted": accepted, "snapshot": self.network_profile_snapshot()}

    def on_awaken(self, node_id: int, uid_hash: int) -> None:
        key = str(node_id)
        if key not in self._node_registry:
            self._node_registry[key] = {
                "uid_hash": f"0x{uid_hash:08x}",
                "first_seen": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            }
            self._write()
