import os
from datetime import datetime

from platformio.public import DeviceMonitorFilterBase

LEVEL_RANK = {
    "T": 0,
    "D": 1,
    "I": 2,
    "W": 3,
    "E": 4,
    "O": 5,
}

LEVEL_NAME = {
    "T": "TRACE",
    "D": "DEBUG",
    "I": "INFO ",
    "W": "WARN ",
    "E": "ERROR",
    "O": "OFF  ",
}


def unescape_field(s: str) -> str:
    """
    Matches DebugLogger::printEscaped().

    Firmware escapes:
      \\  -> \\\\
      tab -> \\t
      LF  -> \\n
      CR  -> \\r
    """
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
                # Unknown escape. Keep the escaped char but drop the slash.
                out.append(nxt)

            i += 2
        else:
            out.append(c)
            i += 1

    return "".join(out)


def parse_sfdbg_line(line: str):
    """
    Parses lines emitted by DebugLogger.h:

      @SFDBG<TAB>v=1<TAB>node=1<TAB>src=sht31<TAB>lvl=I<TAB>seq=42<TAB>t=123456<TAB>msg=...
    """
    line = line.rstrip("\r\n")

    if not line.startswith("@SFDBG\t"):
        return None

    fields = {}

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


def level_allows(record_lvl: str, min_level: str) -> bool:
    return LEVEL_RANK.get(record_lvl, 99) >= LEVEL_RANK.get(min_level, 0)


def parse_csv_env(name: str):
    """
    Allows filters like:

      SFDBG_SRC=i2c
      SFDBG_SRC=i2c,battery,tdma
      SFDBG_NODE=1,2
    """
    value = os.getenv(name, "").strip()

    if not value:
        return set()

    return {x.strip() for x in value.split(",") if x.strip()}


class SmartFiresDebug(DeviceMonitorFilterBase):
    NAME = "smartfires_debug"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

        self._buf = ""

        self._src_filter = parse_csv_env("SFDBG_SRC")
        self._node_filter = parse_csv_env("SFDBG_NODE")

        self._min_level = os.getenv("SFDBG_MIN_LEVEL", "T").strip().upper()
        if self._min_level not in LEVEL_RANK:
            self._min_level = "T"

        self._show_raw = os.getenv("SFDBG_SHOW_RAW", "1").strip() not in (
            "0",
            "false",
            "False",
            "no",
            "NO",
        )
        self._show_header = os.getenv("SFDBG_HEADER", "1").strip() not in (
            "0",
            "false",
            "False",
            "no",
            "NO",
        )
        self._show_level_name = os.getenv("SFDBG_LEVEL_NAME", "0").strip() in (
            "1",
            "true",
            "True",
            "yes",
            "YES",
        )

        self._last_seq_by_node = {}

        if self._show_header:
            print("")
            print("────────────────────────────────────────────────────────────")
            print(" SmartFires structured debug monitor")
            print("────────────────────────────────────────────────────────────")
            print(
                f" node filter : {','.join(sorted(self._node_filter)) if self._node_filter else '*'}"
            )
            print(
                f" src filter  : {','.join(sorted(self._src_filter)) if self._src_filter else '*'}"
            )
            print(f" min level   : {self._min_level}")
            print(f" show raw    : {'yes' if self._show_raw else 'no'}")
            print(" env examples:")
            print("   SFDBG_SRC=i2c pio device monitor -e feather_m0_lora_node_debug")
            print(
                "   SFDBG_SRC=i2c,battery SFDBG_MIN_LEVEL=D pio device monitor -e feather_m0_lora_node_debug"
            )
            print(
                "   SFDBG_SHOW_RAW=0 pio device monitor -e feather_m0_lora_node_debug"
            )
            print("────────────────────────────────────────────────────────────")
            print("")

    def rx(self, text):
        """
        PlatformIO gives chunks, not guaranteed whole lines.
        Buffer until newline so parsing is stable.
        """
        self._buf += text
        output = []

        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            line = line.rstrip("\r")

            formatted = self._format_line(line)
            if formatted:
                output.append(formatted)

        return "".join(output)

    def tx(self, text):
        return text

    def _format_line(self, line: str) -> str:
        if not line:
            return ""

        rec = parse_sfdbg_line(line)

        if rec is None:
            if self._show_raw:
                return f"[raw] {line}\n"
            return ""

        if self._node_filter and rec["node"] not in self._node_filter:
            return ""

        if self._src_filter and rec["src"] not in self._src_filter:
            return ""

        if not level_allows(rec["lvl"], self._min_level):
            return ""

        seq_note = self._check_seq(rec)

        wall = datetime.now().strftime("%H:%M:%S")

        lvl = rec["lvl"]
        lvl_text = LEVEL_NAME.get(lvl, lvl) if self._show_level_name else lvl

        return (
            f"{wall} "
            f"[{lvl_text}] "
            f"node={rec['node']:<3} "
            f"src={rec['src']:<12} "
            f"seq={rec['seq']:<6} "
            f"t={rec['t']:<10} "
            f"{seq_note}"
            f"{rec['msg']}\n"
        )

    def _check_seq(self, rec) -> str:
        """
        Helpful for catching logger resets or missing serial lines.
        This is per node because each node/logger has its own sequence counter.
        """
        node = rec["node"]

        try:
            seq = int(rec["seq"])
        except (TypeError, ValueError):
            return ""

        prev = self._last_seq_by_node.get(node)
        self._last_seq_by_node[node] = seq

        if prev is None:
            return ""

        expected = prev + 1

        if seq == expected:
            return ""

        if seq == 0:
            return "[seq reset] "

        return f"[seq jump {prev}->{seq}] "
