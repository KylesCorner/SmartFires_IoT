"""Parses @SFDBG structured debug lines emitted by logging/DebugLogger.h.

This is a pure-Python port of the parsing logic in
``platformio/monitor/filter_smartfires_debug.py`` (that file depends on
``platformio.public`` and can't be imported from this package). Keep the two
in sync — both must match ``DebugLogger::log()``'s wire format and
``DebugLogger::printEscaped()``'s escaping.
"""

from __future__ import annotations

LEVEL_RANK = {
    "T": 0,
    "D": 1,
    "I": 2,
    "W": 3,
    "E": 4,
    "O": 5,
}


def unescape_field(s: str) -> str:
    """Reverses DebugLogger::printEscaped(): \\\\ -> \\, \\t -> tab, \\n -> LF, \\r -> CR."""
    out = []
    i = 0

    while i < len(s):
        c = s[i]

        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]

            if nxt == "t":
                out.append("\t")
            elif nxt == "n":
                out.append("\n")
            elif nxt == "r":
                out.append("\r")
            elif nxt == "\\":
                out.append("\\")
            else:
                out.append(nxt)

            i += 2
        else:
            out.append(c)
            i += 1

    return "".join(out)


def parse_sfdbg_line(line: str) -> dict | None:
    """Parse a line of the form:

      @SFDBG<TAB>v=1<TAB>node=1<TAB>src=sht31<TAB>lvl=I<TAB>seq=42<TAB>t=123456<TAB>msg=...

    Returns None if *line* isn't a recognized @SFDBG line.
    """
    line = line.rstrip("\r\n")

    if not line.startswith("@SFDBG\t"):
        return None

    fields: dict[str, str] = {}

    for part in line.split("\t")[1:]:
        if "=" not in part:
            continue

        key, value = part.split("=", 1)
        fields[key] = unescape_field(value)

    return {
        "v": fields.get("v", "?"),
        "node": fields.get("node", "?"),
        "src": fields.get("src", "?"),
        "lvl": fields.get("lvl", "?").upper(),
        "seq": fields.get("seq", "-"),
        "t": fields.get("t", "-"),
        "msg": fields.get("msg", ""),
        "raw": line,
    }
