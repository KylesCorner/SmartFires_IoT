"""Per-node Timed duty-cycle window tracking.

The node brackets each wake with PKT_WINDOW_BEGIN / PKT_WINDOW_END rather than
flagging the bundles at the edges (the retired WINDOW_FIRST/WINDOW_LAST header
bits). That moves window bookkeeping off the packets themselves and onto this
side of the link, which is what this module does.

Why the edge has to do the work: ``window_last`` used to sit on the last bundle
of a window, but a streaming consumer cannot know a bundle is the last one until
the window has already closed. The explicit END frame carries the close instant
instead, so ``window_id`` — stamped on every row of the window — becomes the
thing to group on, and it keeps working when a marker is lost.

Continuous mode emits no markers at all, so every node starts with no open
window and rows simply carry ``window_id = None``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional


@dataclass
class _NodeWindow:
    window_id: Optional[int] = None
    # Cleared by the first telemetry row attributed to this window, so exactly
    # one row per window carries window_first.
    awaiting_first_row: bool = False
    open: bool = False


@dataclass
class WindowTracker:
    """Tracks which duty-cycle window each node's telemetry belongs to."""

    _nodes: dict[int, _NodeWindow] = field(default_factory=dict)

    def _node(self, node_id: int) -> _NodeWindow:
        return self._nodes.setdefault(int(node_id), _NodeWindow())

    def on_window_begin(self, node_id: int, window_id: int) -> None:
        node = self._node(node_id)
        node.window_id = int(window_id)
        node.awaiting_first_row = True
        node.open = True

    def on_window_end(self, node_id: int, window_id: int) -> None:
        node = self._node(node_id)
        # Trust the END's own id even if the matching BEGIN was lost — it is the
        # node's authoritative statement about which window just closed.
        node.window_id = int(window_id)
        node.open = False
        node.awaiting_first_row = False

    def on_reboot(self, node_id: int) -> None:
        """AWAKEN: the node restarted, so its window counter restarted too.

        Without this, telemetry from after a watchdog reboot would be attributed
        to whatever window was open before it — and a node that reboots mid-sleep
        never sends the END for that window at all.
        """
        self._nodes.pop(int(node_id), None)

    def annotate(self, node_id: int, row: dict) -> dict:
        """Stamp window_id/window_first onto one telemetry row, in place."""
        node = self._nodes.get(int(node_id))
        if node is None or node.window_id is None:
            return row

        row["window_id"] = node.window_id

        if node.awaiting_first_row and node.open:
            row["window_first"] = 1
            node.awaiting_first_row = False

        return row

    def current_window_id(self, node_id: int) -> Optional[int]:
        node = self._nodes.get(int(node_id))
        return node.window_id if node is not None else None
