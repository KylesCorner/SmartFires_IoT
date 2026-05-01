#!/usr/bin/env python3

import argparse
import re
import threading
import time
from collections import deque

import pandas as pd
import panel as pn
import serial
from bokeh.models import ColumnDataSource, FactorRange, HoverTool
from bokeh.palettes import Viridis256
from bokeh.plotting import figure
from bokeh.transform import linear_cmap

pn.extension("tabulator")

RX_RE = re.compile(
    r"RX\s+"
    r"count=(?P<count>\d+)\s+"
    r"t_ms=(?P<t_ms>\d+)\s+"
    r"dt_ms=(?P<dt_ms>\d+)\s+"
    r"len=(?P<length>\d+)\s+"
    r"rssi=(?P<rssi>-?\d+)\s+"
    r"rh_to=(?P<rh_to>\d+)\s+"
    r"rh_from=(?P<rh_from>\d+)\s+"
    r"rh_id=(?P<rh_id>\d+)\s+"
    r"rh_flags=0x(?P<rh_flags>[0-9A-Fa-f]+)\s+"
    r"decode=(?P<decode>\S+)"
)


class PacketStore:
    def __init__(self, max_rows=5000):
        self.rows = deque(maxlen=max_rows)
        self.raw_lines = deque(maxlen=200)
        self.lock = threading.Lock()

    def add_packet(self, row):
        with self.lock:
            self.rows.append(row)

    def add_raw(self, line):
        with self.lock:
            self.raw_lines.append(
                {
                    "time": pd.Timestamp.now(),
                    "line": line,
                }
            )

    def dataframe(self):
        with self.lock:
            if not self.rows:
                return empty_packet_dataframe()
            return pd.DataFrame(list(self.rows))

    def raw_dataframe(self):
        with self.lock:
            if not self.raw_lines:
                return pd.DataFrame(columns=["time", "line"])
            return pd.DataFrame(list(self.raw_lines))


def empty_packet_dataframe():
    return pd.DataFrame(
        columns=[
            "wall_time",
            "count",
            "t_ms",
            "dt_ms",
            "length",
            "rssi",
            "snr",
            "rh_to",
            "rh_from",
            "rh_id",
            "rh_flags",
            "decode",
            "type_name",
            "type_id",
            "node",
            "seq",
            "payload_len",
            "session_time_ms",
            "uptime_ms",
            "tdma_clock_ms",
            "tdma_frame",
            "tdma_slot",
            "tdma_slot_phase_ms",
            "tdma_slot_progress",
            "expected_node",
            "slot_match",
            "hex",
            "ascii",
            "raw",
        ]
    )


def parse_hex_bytes(hex_payload):
    if not hex_payload:
        return b""

    try:
        return bytes(int(x, 16) for x in hex_payload.split())
    except ValueError:
        return b""
def decode_full_state_timing_from_hex(hex_payload, type_name):
    """
    SmartFires packet format assumption:

    bytes 0..3:
      magic, pkt_type, node_id, seq

    FULL_STATE and BUNDLE both begin with FullStatePayload after the header.

    FullStatePayload begins:
      session_time: uint32 little-endian
    """
    if type_name not in ("FULL_STATE", "BUNDLE"):
        return None, None

    raw = parse_hex_bytes(hex_payload)

    if len(raw) < 8:
        return None, None

    session_time_ms = int.from_bytes(raw[4:8], byteorder="little", signed=False)

    # Current refactor/only-feather FullStatePayload has no uptime_ms.
    return session_time_ms, None

# def decode_full_state_timing_from_hex(hex_payload, type_name):
#     """
#     SmartFires packet format assumption:
#
#     bytes 0..3:
#       magic, pkt_type, node_id, seq
#
#     FULL_STATE payload starts at byte 4.
#
#     FullStatePayload begins:
#       session_time_ms: uint32 little-endian
#       uptime_ms:       uint32 little-endian
#     """
#     if type_name != "FULL_STATE":
#         return None, None
#
#     raw = parse_hex_bytes(hex_payload)
#
#     if len(raw) < 12:
#         return None, None
#
#     session_time_ms = int.from_bytes(raw[4:8], byteorder="little", signed=False)
#     uptime_ms = int.from_bytes(raw[8:12], byteorder="little", signed=False)
#
#     return session_time_ms, uptime_ms


def parse_rx_line(line):
    line = line.strip()

    m = RX_RE.search(line)
    if not m:
        return None

    g = m.groupdict()

    def find_int(name, default=None):
        mm = re.search(rf"{name}=(-?\d+)", line)
        return int(mm.group(1)) if mm else default

    def find_hex(name, default=0):
        mm = re.search(rf"{name}=0x([0-9A-Fa-f]+)", line)
        return int(mm.group(1), 16) if mm else default

    type_name = "UNKNOWN"
    type_id = None
    type_match = re.search(r"type=([A-Z_]+)\((\d+)\)", line)
    if type_match:
        type_name = type_match.group(1)
        type_id = int(type_match.group(2))

    hex_payload = ""
    hex_match = re.search(r"hex=\[([^\]]*)\]", line)
    if hex_match:
        hex_payload = hex_match.group(1)

    ascii_payload = ""
    ascii_match = re.search(r'ascii="(.*)"', line)
    if ascii_match:
        ascii_payload = ascii_match.group(1)

    session_time_ms = find_int("session_time_ms")
    uptime_ms = find_int("uptime_ms")

    if session_time_ms is None or uptime_ms is None:
        decoded_session_ms, decoded_uptime_ms = decode_full_state_timing_from_hex(
            hex_payload,
            type_name,
        )

        if session_time_ms is None:
            session_time_ms = decoded_session_ms

        if uptime_ms is None:
            uptime_ms = decoded_uptime_ms

    return {
        "wall_time": pd.Timestamp.now(),
        "count": int(g["count"]),
        "t_ms": int(g["t_ms"]),
        "dt_ms": int(g["dt_ms"]),
        "length": int(g["length"]),
        "rssi": int(g["rssi"]),
        "snr": find_int("snr"),
        "rh_to": int(g["rh_to"]),
        "rh_from": int(g["rh_from"]),
        "rh_id": int(g["rh_id"]),
        "rh_flags": find_hex("rh_flags"),
        "decode": g["decode"],
        "type_name": type_name,
        "type_id": type_id,
        "node": find_int("node"),
        "seq": find_int("seq"),
        "payload_len": find_int("payload_len"),
        "session_time_ms": session_time_ms,
        "uptime_ms": uptime_ms,
        "hex": hex_payload,
        "ascii": ascii_payload,
        "raw": line,
    }


def compute_tdma_fields(row, tdma_epoch_ms, slot_ms, num_slots, clock_source):
    """
    Adds TDMA frame/slot metadata to one parsed packet row.

    clock_source options:
      session_time: use SmartFires session_time_ms only
      sniffer: use sniffer-local t_ms only
      auto: prefer session_time_ms, fall back to sniffer t_ms
    """
    packet_clock_ms = None

    if clock_source == "session_time":
        packet_clock_ms = row.get("session_time_ms")
    elif clock_source == "sniffer":
        packet_clock_ms = row.get("t_ms")
    else:
        packet_clock_ms = row.get("session_time_ms")
        if packet_clock_ms is None or pd.isna(packet_clock_ms):
            packet_clock_ms = row.get("t_ms")

    if packet_clock_ms is None or pd.isna(packet_clock_ms):
        return {
            "tdma_clock_ms": None,
            "tdma_frame": None,
            "tdma_slot": None,
            "tdma_slot_phase_ms": None,
            "tdma_slot_progress": None,
        }

    slot_clock_ms = int(packet_clock_ms) - int(tdma_epoch_ms)

    if slot_clock_ms < 0:
        return {
            "tdma_clock_ms": int(packet_clock_ms),
            "tdma_frame": None,
            "tdma_slot": None,
            "tdma_slot_phase_ms": None,
            "tdma_slot_progress": None,
        }

    frame_ms = int(slot_ms) * int(num_slots)

    frame = slot_clock_ms // frame_ms
    frame_phase_ms = slot_clock_ms % frame_ms
    slot = frame_phase_ms // int(slot_ms)
    slot_phase_ms = frame_phase_ms % int(slot_ms)

    return {
        "tdma_clock_ms": int(packet_clock_ms),
        "tdma_frame": int(frame),
        "tdma_slot": int(slot),
        "tdma_slot_phase_ms": int(slot_phase_ms),
        "tdma_slot_progress": float(slot_phase_ms) / float(slot_ms),
    }


def expected_node_for_slot(slot, node_offset, slot_node_map):
    if slot is None or pd.isna(slot):
        return None

    slot_int = int(slot)

    if slot_node_map:
        return slot_node_map.get(slot_int)

    return slot_int + int(node_offset)


def parse_slot_node_map(raw_map):
    """
    Parses strings like:

      0:1,1:2,2:5,3:7

    Meaning:
      slot 0 -> node 1
      slot 1 -> node 2
      slot 2 -> node 5
      slot 3 -> node 7
    """
    if not raw_map:
        return {}

    result = {}

    for item in raw_map.split(","):
        item = item.strip()
        if not item:
            continue

        if ":" not in item:
            raise ValueError(
                f"Invalid slot map entry {item!r}. Expected format like 0:1,1:2"
            )

        slot_raw, node_raw = item.split(":", 1)
        result[int(slot_raw.strip())] = int(node_raw.strip())

    return result


def serial_reader_loop(port, baud, store, stop_event, verbose=False):
    while not stop_event.is_set():
        try:
            with serial.Serial(port, baudrate=baud, timeout=1) as ser:
                print(f"Connected to {port} @ {baud}")

                while not stop_event.is_set():
                    raw = ser.readline()
                    if not raw:
                        continue

                    try:
                        line = raw.decode("utf-8", errors="replace").strip()
                    except Exception:
                        continue

                    if not line:
                        continue

                    store.add_raw(line)

                    if not line.startswith("RX "):
                        continue

                    row = parse_rx_line(line)
                    if row is not None:
                        if verbose:
                            print(
                                f"PARSED RX count={row['count']} "
                                f"from={row['rh_from']} "
                                f"node={row['node']} "
                                f"rssi={row['rssi']}"
                            )
                        store.add_packet(row)
                    else:
                        print(f"UNPARSED RX LINE: {line}")

        except serial.SerialException as e:
            print(f"Serial error: {e}")
            time.sleep(2)
        except Exception as e:
            print(f"Reader error: {e}")
            time.sleep(2)


def make_rssi_plot(source):
    p = figure(
        title="RSSI over time",
        x_axis_type="datetime",
        height=320,
        sizing_mode="stretch_width",
        tools="pan,wheel_zoom,box_zoom,reset,save",
    )
    p.scatter(
        x="wall_time",
        y="rssi",
        source=source,
        size=7,
    )
    p.xaxis.axis_label = "Time"
    p.yaxis.axis_label = "RSSI (dBm)"
    p.add_tools(
        HoverTool(
            tooltips=[
                ("time", "@time_text"),
                ("rssi", "@rssi"),
                ("rh_from", "@rh_from"),
                ("rh_to", "@rh_to"),
                ("node", "@node"),
                ("type", "@type_name"),
                ("seq", "@seq"),
                ("len", "@length"),
                ("tdma frame", "@tdma_frame"),
                ("tdma slot", "@tdma_slot"),
                ("slot phase ms", "@tdma_slot_phase_ms"),
            ]
        )
    )
    return p


def make_dt_plot(source):
    p = figure(
        title="Inter-packet gap",
        x_axis_type="datetime",
        height=320,
        sizing_mode="stretch_width",
        tools="pan,wheel_zoom,box_zoom,reset,save",
    )
    p.line(
        x="wall_time",
        y="dt_ms",
        source=source,
        line_width=2,
    )
    p.scatter(
        x="wall_time",
        y="dt_ms",
        source=source,
        size=5,
    )
    p.xaxis.axis_label = "Time"
    p.yaxis.axis_label = "dt_ms"
    p.add_tools(
        HoverTool(
            tooltips=[
                ("time", "@time_text"),
                ("dt_ms", "@dt_ms"),
                ("rh_from", "@rh_from"),
                ("rh_to", "@rh_to"),
                ("node", "@node"),
                ("type", "@type_name"),
                ("seq", "@seq"),
                ("tdma frame", "@tdma_frame"),
                ("tdma slot", "@tdma_slot"),
            ]
        )
    )
    return p


def make_slot_phase_plot(source):
    p = figure(
        title="TDMA slot phase over time",
        x_axis_type="datetime",
        height=320,
        sizing_mode="stretch_width",
        tools="pan,wheel_zoom,box_zoom,reset,save",
    )
    p.scatter(
        x="wall_time",
        y="tdma_slot_phase_ms",
        source=source,
        size=7,
    )
    p.xaxis.axis_label = "Time"
    p.yaxis.axis_label = "Slot phase ms"
    p.add_tools(
        HoverTool(
            tooltips=[
                ("time", "@time_text"),
                ("node", "@node"),
                ("seq", "@seq"),
                ("tdma frame", "@tdma_frame"),
                ("tdma slot", "@tdma_slot"),
                ("slot phase ms", "@tdma_slot_phase_ms"),
                ("expected node", "@expected_node"),
                ("slot match", "@slot_match"),
            ]
        )
    )
    return p


def make_bar_plot(title, x_label, y_label, source):
    p = figure(
        title=title,
        x_range=FactorRange(factors=[]),
        height=320,
        sizing_mode="stretch_width",
        tools="pan,wheel_zoom,box_zoom,reset,save",
    )
    p.vbar(
        x="category",
        top="packets",
        width=0.8,
        source=source,
    )
    p.xaxis.axis_label = x_label
    p.yaxis.axis_label = y_label
    p.xaxis.major_label_orientation = 0.8
    p.add_tools(
        HoverTool(
            tooltips=[
                ("category", "@category"),
                ("packets", "@packets"),
            ]
        )
    )
    return p


def make_tdma_heatmap(source, num_slots, high_packets_per_cell):
    slots = [str(i) for i in range(num_slots)]

    p = figure(
        title="TDMA slot activity over time",
        x_axis_type="datetime",
        y_range=list(reversed(slots)),
        height=390,
        sizing_mode="stretch_width",
        tools="pan,wheel_zoom,box_zoom,reset,save",
    )

    mapper = linear_cmap(
        field_name="packets",
        palette=Viridis256,
        low=0,
        high=high_packets_per_cell,
    )

    p.rect(
        x="bucket_time",
        y="slot_label",
        width="bucket_width_ms",
        height=0.9,
        source=source,
        fill_color=mapper,
        line_color=None,
    )

    p.xaxis.axis_label = "Time"
    p.yaxis.axis_label = "TDMA slot"

    p.add_tools(
        HoverTool(
            tooltips=[
                ("time", "@time_text"),
                ("slot", "@slot_label"),
                ("packets", "@packets"),
                ("nodes", "@nodes"),
                ("bad slot packets", "@bad_slot_packets"),
                ("avg RSSI", "@avg_rssi"),
            ]
        )
    )

    return p


def dataframe_for_plot(df, plot_window):
    columns = {
        "wall_time": [],
        "time_text": [],
        "rssi": [],
        "dt_ms": [],
        "rh_from": [],
        "rh_to": [],
        "node": [],
        "type_name": [],
        "seq": [],
        "length": [],
        "tdma_frame": [],
        "tdma_slot": [],
        "tdma_slot_phase_ms": [],
        "expected_node": [],
        "slot_match": [],
    }

    if df.empty:
        return pd.DataFrame(columns)

    plot_df = df.sort_values("wall_time").tail(plot_window).copy()
    plot_df["time_text"] = plot_df["wall_time"].dt.strftime("%H:%M:%S")
    plot_df["type_name"] = plot_df["type_name"].fillna("UNKNOWN")
    plot_df["seq"] = plot_df["seq"].fillna(-1).astype(int)
    plot_df["node"] = plot_df["node"].fillna(-1).astype(int)
    plot_df["tdma_frame"] = plot_df["tdma_frame"].fillna(-1).astype(int)
    plot_df["tdma_slot"] = plot_df["tdma_slot"].fillna(-1).astype(int)
    plot_df["tdma_slot_phase_ms"] = plot_df["tdma_slot_phase_ms"].fillna(-1).astype(int)
    plot_df["expected_node"] = plot_df["expected_node"].fillna(-1).astype(int)
    plot_df["slot_match"] = plot_df["slot_match"].fillna(False).astype(bool)

    return plot_df


def build_dashboard(
    store,
    plot_window,
    update_ms,
    tdma_slot_ms,
    tdma_num_slots,
    tdma_epoch_ms,
    tdma_clock_source,
    tdma_node_offset,
    tdma_slot_node_map,
    tdma_heatmap_bucket,
):
    tdma_state = {
        "epoch_ms": tdma_epoch_ms,
    }

    total_packets = pn.indicators.Number(
        name="Total Packets",
        value=0,
        format="{value}",
    )
    last_60s = pn.indicators.Number(
        name="Packets Last 60s",
        value=0,
        format="{value}",
    )
    unique_sources = pn.indicators.Number(
        name="Unique RH Sources",
        value=0,
        format="{value}",
    )
    unique_nodes = pn.indicators.Number(
        name="Unique Nodes",
        value=0,
        format="{value}",
    )
    avg_rssi = pn.indicators.Number(
        name="Avg RSSI Last 100",
        value=0,
        format="{value:.1f}",
    )
    bad_slot_packets = pn.indicators.Number(
        name="Bad Slot Packets",
        value=0,
        format="{value}",
    )

    latest_packet = pn.widgets.TextAreaInput(
        name="Latest Packet",
        value="Waiting for packets...",
        disabled=True,
        height=110,
        sizing_mode="stretch_width",
    )

    decode_summary = pn.pane.Markdown(
        "**Decode counts:** waiting...",
        height=95,
        sizing_mode="stretch_width",
    )

    tdma_summary = pn.pane.Markdown(
        "**TDMA:** waiting...",
        height=110,
        sizing_mode="stretch_width",
    )

    recent_packets = pn.widgets.Tabulator(
        pd.DataFrame(),
        disabled=True,
        pagination="local",
        page_size=15,
        sizing_mode="stretch_width",
        height=430,
        sorters=[
            {"field": "wall_time", "dir": "desc"},
        ],
    )

    tdma_activity = pn.widgets.Tabulator(
        pd.DataFrame(),
        disabled=True,
        pagination="local",
        page_size=20,
        sizing_mode="stretch_width",
        height=430,
        sorters=[
            {"field": "tdma_frame", "dir": "desc"},
            {"field": "tdma_slot", "dir": "asc"},
        ],
    )

    recent_raw = pn.widgets.Tabulator(
        pd.DataFrame(),
        disabled=True,
        pagination="local",
        page_size=15,
        sizing_mode="stretch_width",
        height=430,
    )

    plot_source = ColumnDataSource(
        data={
            "wall_time": [],
            "time_text": [],
            "rssi": [],
            "dt_ms": [],
            "rh_from": [],
            "rh_to": [],
            "node": [],
            "type_name": [],
            "seq": [],
            "length": [],
            "tdma_frame": [],
            "tdma_slot": [],
            "tdma_slot_phase_ms": [],
            "expected_node": [],
            "slot_match": [],
        }
    )

    source_bar_source = ColumnDataSource(data={"category": [], "packets": []})
    type_bar_source = ColumnDataSource(data={"category": [], "packets": []})
    slot_bar_source = ColumnDataSource(data={"category": [], "packets": []})

    tdma_heatmap_source = ColumnDataSource(
        data={
            "bucket_time": [],
            "time_text": [],
            "slot_label": [],
            "packets": [],
            "nodes": [],
            "bad_slot_packets": [],
            "avg_rssi": [],
            "bucket_width_ms": [],
        }
    )

    rssi_plot = make_rssi_plot(plot_source)
    dt_plot = make_dt_plot(plot_source)
    slot_phase_plot = make_slot_phase_plot(plot_source)

    tdma_heatmap = make_tdma_heatmap(
        tdma_heatmap_source,
        num_slots=tdma_num_slots,
        high_packets_per_cell=5,
    )

    source_bar = make_bar_plot(
        "Packets by RadioHead source",
        "RH source",
        "Packets",
        source_bar_source,
    )
    type_bar = make_bar_plot(
        "Packets by packet type",
        "Packet type",
        "Packets",
        type_bar_source,
    )
    slot_bar = make_bar_plot(
        "Packets by TDMA slot",
        "TDMA slot",
        "Packets",
        slot_bar_source,
    )

    plots = pn.Column(
        pn.pane.Bokeh(tdma_heatmap, sizing_mode="stretch_width"),
        pn.pane.Bokeh(slot_phase_plot, sizing_mode="stretch_width"),
        pn.pane.Bokeh(rssi_plot, sizing_mode="stretch_width"),
        pn.pane.Bokeh(dt_plot, sizing_mode="stretch_width"),
        pn.Row(
            pn.pane.Bokeh(slot_bar, sizing_mode="stretch_width"),
            pn.pane.Bokeh(source_bar, sizing_mode="stretch_width"),
            sizing_mode="stretch_width",
        ),
        pn.pane.Bokeh(type_bar, sizing_mode="stretch_width"),
        sizing_mode="stretch_width",
    )

    tabs = pn.Tabs(
        ("Plots", plots),
        ("TDMA Slot Activity", tdma_activity),
        ("Recent Parsed Packets", recent_packets),
        ("Recent Raw Serial Lines", recent_raw),
        dynamic=False,
        sizing_mode="stretch_width",
    )

    last_rendered_signature = {"value": None}

    def choose_epoch(df):
        if tdma_state["epoch_ms"] is not None:
            return

        if df.empty:
            return

        if tdma_clock_source in ("auto", "session_time"):
            session_times = df["session_time_ms"].dropna()
            if not session_times.empty:
                tdma_state["epoch_ms"] = int(session_times.iloc[0])
                return

        tdma_state["epoch_ms"] = int(df.iloc[0]["t_ms"])

    def attach_tdma_fields(df):
        if df.empty:
            return df

        choose_epoch(df)

        if tdma_state["epoch_ms"] is None:
            return df

        tdma_rows = []
        for _, packet_row in df.iterrows():
            tdma = compute_tdma_fields(
                packet_row,
                tdma_epoch_ms=tdma_state["epoch_ms"],
                slot_ms=tdma_slot_ms,
                num_slots=tdma_num_slots,
                clock_source=tdma_clock_source,
            )
            tdma_rows.append(tdma)

        tdma_df = pd.DataFrame(tdma_rows, index=df.index)

        for col in tdma_df.columns:
            df[col] = tdma_df[col]

        df["expected_node"] = df["tdma_slot"].apply(
            lambda s: expected_node_for_slot(
                s,
                node_offset=tdma_node_offset,
                slot_node_map=tdma_slot_node_map,
            )
        )

        def slot_matches(row):
            node = row.get("node")
            expected_node = row.get("expected_node")

            if node is None or expected_node is None:
                return None

            if pd.isna(node) or pd.isna(expected_node):
                return None

            return int(node) == int(expected_node)

        df["slot_match"] = df.apply(slot_matches, axis=1)

        return df

    def make_tdma_activity_df(df):
        if df.empty:
            return pd.DataFrame(
                columns=[
                    "tdma_frame",
                    "tdma_slot",
                    "packets",
                    "nodes",
                    "expected_node",
                    "avg_rssi",
                    "min_phase_ms",
                    "max_phase_ms",
                    "bad_slot_packets",
                    "slot_matches",
                ]
            )

        working = df.dropna(subset=["tdma_frame", "tdma_slot"]).copy()

        if working.empty:
            return pd.DataFrame(
                columns=[
                    "tdma_frame",
                    "tdma_slot",
                    "packets",
                    "nodes",
                    "expected_node",
                    "avg_rssi",
                    "min_phase_ms",
                    "max_phase_ms",
                    "bad_slot_packets",
                    "slot_matches",
                ]
            )

        def join_nodes(x):
            values = []
            for v in x.dropna():
                values.append(str(int(v)))
            return ",".join(sorted(set(values), key=lambda s: int(s)))

        def count_bad_slots(x):
            return int((x == False).sum())

        def count_good_slots(x):
            return int((x == True).sum())

        activity = (
            working.groupby(["tdma_frame", "tdma_slot"])
            .agg(
                packets=("count", "count"),
                nodes=("node", join_nodes),
                expected_node=("expected_node", "first"),
                avg_rssi=("rssi", "mean"),
                min_phase_ms=("tdma_slot_phase_ms", "min"),
                max_phase_ms=("tdma_slot_phase_ms", "max"),
                bad_slot_packets=("slot_match", count_bad_slots),
                slot_matches=("slot_match", count_good_slots),
            )
            .reset_index()
            .sort_values(["tdma_frame", "tdma_slot"], ascending=[False, True])
            .head(500)
        )

        activity["tdma_frame"] = activity["tdma_frame"].astype(int)
        activity["tdma_slot"] = activity["tdma_slot"].astype(int)
        activity["expected_node"] = activity["expected_node"].fillna(-1).astype(int)
        activity["avg_rssi"] = activity["avg_rssi"].round(1)
        activity["min_phase_ms"] = activity["min_phase_ms"].fillna(-1).astype(int)
        activity["max_phase_ms"] = activity["max_phase_ms"].fillna(-1).astype(int)

        return activity

    def update_tdma_heatmap(df):
        if df.empty:
            tdma_heatmap_source.data = {
                "bucket_time": [],
                "time_text": [],
                "slot_label": [],
                "packets": [],
                "nodes": [],
                "bad_slot_packets": [],
                "avg_rssi": [],
                "bucket_width_ms": [],
            }
            return

        tdma_plot_df = df.dropna(subset=["tdma_slot"]).copy()

        if tdma_plot_df.empty:
            tdma_heatmap_source.data = {
                "bucket_time": [],
                "time_text": [],
                "slot_label": [],
                "packets": [],
                "nodes": [],
                "bad_slot_packets": [],
                "avg_rssi": [],
                "bucket_width_ms": [],
            }
            return

        frame_ms = tdma_slot_ms * tdma_num_slots

        if tdma_heatmap_bucket == "slot":
            bucket_ms = tdma_slot_ms
        else:
            bucket_ms = frame_ms

        tdma_plot_df = tdma_plot_df.sort_values("wall_time").tail(plot_window).copy()

        first_time = tdma_plot_df["wall_time"].min()

        tdma_plot_df["bucket_index"] = (
            (tdma_plot_df["wall_time"] - first_time).dt.total_seconds()
            * 1000.0
            / bucket_ms
        ).astype(int)

        tdma_plot_df["bucket_time"] = first_time + pd.to_timedelta(
            tdma_plot_df["bucket_index"] * bucket_ms,
            unit="ms",
        )

        def join_nodes(x):
            values = []
            for v in x.dropna():
                values.append(str(int(v)))
            return ",".join(sorted(set(values), key=lambda s: int(s)))

        heat_df = (
            tdma_plot_df.assign(
                slot_label=tdma_plot_df["tdma_slot"].astype(int).astype(str)
            )
            .groupby(["bucket_time", "slot_label"])
            .agg(
                packets=("count", "count"),
                nodes=("node", join_nodes),
                bad_slot_packets=("slot_match", lambda x: int((x == False).sum())),
                avg_rssi=("rssi", "mean"),
            )
            .reset_index()
        )

        heat_df["time_text"] = heat_df["bucket_time"].dt.strftime("%H:%M:%S")
        heat_df["avg_rssi"] = heat_df["avg_rssi"].round(1)
        heat_df["bucket_width_ms"] = bucket_ms

        tdma_heatmap_source.data = {
            "bucket_time": heat_df["bucket_time"].tolist(),
            "time_text": heat_df["time_text"].tolist(),
            "slot_label": heat_df["slot_label"].tolist(),
            "packets": heat_df["packets"].tolist(),
            "nodes": heat_df["nodes"].tolist(),
            "bad_slot_packets": heat_df["bad_slot_packets"].tolist(),
            "avg_rssi": heat_df["avg_rssi"].tolist(),
            "bucket_width_ms": heat_df["bucket_width_ms"].tolist(),
        }

    def update():
        df = store.dataframe()
        raw_df = store.raw_dataframe()

        if df.empty:
            total_packets.value = 0
            last_60s.value = 0
            unique_sources.value = 0
            unique_nodes.value = 0
            avg_rssi.value = 0
            bad_slot_packets.value = 0
            latest_packet.value = "Waiting for packets..."
            decode_summary.object = "**Decode counts:** waiting..."
            tdma_summary.object = "**TDMA:** waiting..."

            empty_plot = dataframe_for_plot(df, plot_window)
            plot_source.data = empty_plot.to_dict(orient="list")

            source_bar_source.data = {"category": [], "packets": []}
            source_bar.x_range.factors = []

            type_bar_source.data = {"category": [], "packets": []}
            type_bar.x_range.factors = []

            slot_bar_source.data = {"category": [], "packets": []}
            slot_bar.x_range.factors = []

            tdma_heatmap_source.data = {
                "bucket_time": [],
                "time_text": [],
                "slot_label": [],
                "packets": [],
                "nodes": [],
                "bad_slot_packets": [],
                "avg_rssi": [],
                "bucket_width_ms": [],
            }

            recent_packets.value = pd.DataFrame()
            tdma_activity.value = pd.DataFrame()
        else:
            df = df.sort_values("wall_time").copy()
            df = attach_tdma_fields(df)

            latest_count = int(df.iloc[-1]["count"])
            latest_raw_time = None
            if not raw_df.empty:
                latest_raw_time = str(raw_df.iloc[-1]["time"])

            signature = (latest_count, latest_raw_time)

            # Avoid doing full UI work if nothing new has arrived.
            if last_rendered_signature["value"] == signature:
                return

            last_rendered_signature["value"] = signature

            now = pd.Timestamp.now()
            cutoff = now - pd.Timedelta(seconds=60)
            recent_60 = df[df["wall_time"] >= cutoff]

            total_packets.value = int(len(df))
            last_60s.value = int(len(recent_60))
            unique_sources.value = int(df["rh_from"].nunique())
            unique_nodes.value = int(df["node"].dropna().nunique())
            avg_rssi.value = float(df["rssi"].tail(100).mean())

            bad_count = int((df["slot_match"] == False).sum())
            bad_slot_packets.value = bad_count

            latest_packet.value = str(df.iloc[-1]["raw"])

            decode_counts = df["decode"].value_counts().to_dict()
            type_counts = df["type_name"].value_counts().to_dict()

            decode_summary.object = (
                f"**Decode counts:** `{decode_counts}`  \n"
                f"**Type counts:** `{type_counts}`"
            )

            latest = df.iloc[-1]
            tdma_summary.object = (
                f"**TDMA config**  \n"
                f"- slot width: `{tdma_slot_ms} ms`  \n"
                f"- slots/frame: `{tdma_num_slots}`  \n"
                f"- frame width: `{tdma_slot_ms * tdma_num_slots} ms`  \n"
                f"- clock source: `{tdma_clock_source}`  \n"
                f"- epoch ms: `{tdma_state['epoch_ms']}`  \n\n"
                f"**Latest packet TDMA**  \n"
                f"- frame: `{latest.get('tdma_frame')}`  \n"
                f"- slot: `{latest.get('tdma_slot')}`  \n"
                f"- slot phase: `{latest.get('tdma_slot_phase_ms')} ms`  \n"
                f"- node: `{latest.get('node')}`  \n"
                f"- expected node: `{latest.get('expected_node')}`  \n"
                f"- slot match: `{latest.get('slot_match')}`"
            )

            plot_df = dataframe_for_plot(df, plot_window)

            plot_data = {
                "wall_time": plot_df["wall_time"].tolist(),
                "time_text": plot_df["time_text"].tolist(),
                "rssi": plot_df["rssi"].tolist(),
                "dt_ms": plot_df["dt_ms"].tolist(),
                "rh_from": plot_df["rh_from"].tolist(),
                "rh_to": plot_df["rh_to"].tolist(),
                "node": plot_df["node"].tolist(),
                "type_name": plot_df["type_name"].tolist(),
                "seq": plot_df["seq"].tolist(),
                "length": plot_df["length"].tolist(),
                "tdma_frame": plot_df["tdma_frame"].tolist(),
                "tdma_slot": plot_df["tdma_slot"].tolist(),
                "tdma_slot_phase_ms": plot_df["tdma_slot_phase_ms"].tolist(),
                "expected_node": plot_df["expected_node"].tolist(),
                "slot_match": plot_df["slot_match"].tolist(),
            }
            plot_source.data = plot_data

            per_source = (
                plot_df.assign(source=plot_df["rh_from"].astype(str))
                .groupby("source")
                .size()
                .reset_index(name="packets")
                .sort_values("source")
            )
            source_categories = per_source["source"].tolist()
            source_bar.x_range.factors = source_categories
            source_bar_source.data = {
                "category": source_categories,
                "packets": per_source["packets"].tolist(),
            }

            per_type = (
                plot_df.assign(type_label=plot_df["type_name"].fillna("UNKNOWN"))
                .groupby("type_label")
                .size()
                .reset_index(name="packets")
                .sort_values("packets", ascending=False)
            )
            type_categories = per_type["type_label"].tolist()
            type_bar.x_range.factors = type_categories
            type_bar_source.data = {
                "category": type_categories,
                "packets": per_type["packets"].tolist(),
            }

            per_slot = (
                df.dropna(subset=["tdma_slot"])
                .assign(slot_label=df["tdma_slot"].dropna().astype(int).astype(str))
                .groupby("slot_label")
                .size()
                .reset_index(name="packets")
            )

            if not per_slot.empty:
                per_slot["slot_sort"] = per_slot["slot_label"].astype(int)
                per_slot = per_slot.sort_values("slot_sort")

            slot_categories = per_slot["slot_label"].tolist()
            slot_bar.x_range.factors = slot_categories
            slot_bar_source.data = {
                "category": slot_categories,
                "packets": per_slot["packets"].tolist(),
            }

            update_tdma_heatmap(df)

            tdma_activity.value = make_tdma_activity_df(df)

            display_cols = [
                "wall_time",
                "count",
                "rh_from",
                "rh_to",
                "decode",
                "type_name",
                "node",
                "seq",
                "length",
                "payload_len",
                "rssi",
                "dt_ms",
                "session_time_ms",
                "uptime_ms",
                "tdma_clock_ms",
                "tdma_frame",
                "tdma_slot",
                "tdma_slot_phase_ms",
                "expected_node",
                "slot_match",
                "hex",
            ]

            available_display_cols = [c for c in display_cols if c in df.columns]

            display_df = (
                df[available_display_cols]
                .tail(200)
                .sort_values("wall_time", ascending=False)
                .copy()
            )
            display_df["wall_time"] = display_df["wall_time"].dt.strftime("%H:%M:%S")
            recent_packets.value = display_df

        if raw_df.empty:
            recent_raw.value = pd.DataFrame(columns=["time", "line"])
        else:
            raw_display = raw_df.sort_values("time", ascending=False).head(100).copy()
            raw_display["time"] = raw_display["time"].dt.strftime("%H:%M:%S")
            recent_raw.value = raw_display

    dashboard = pn.Column(
        pn.pane.Markdown("# SmartFires LoRa Sniffer Dashboard"),
        pn.Row(
            total_packets,
            last_60s,
            unique_sources,
            unique_nodes,
            avg_rssi,
            bad_slot_packets,
            sizing_mode="stretch_width",
        ),
        tabs,
        pn.Accordion(
            (
                "Latest Packet / Decode Summary / TDMA Summary",
                pn.Column(latest_packet, decode_summary, tdma_summary),
            ),
            active=[],
            sizing_mode="stretch_width",
        ),
        sizing_mode="stretch_width",
    )

    update()
    pn.state.add_periodic_callback(update, period=update_ms, start=True)

    return dashboard


def main():
    parser = argparse.ArgumentParser(
        description="Live dashboard for SmartFires LoRa sniffer output"
    )
    parser.add_argument(
        "--serial-port",
        required=True,
        help="Serial port, e.g. /dev/ttyACM0, /dev/ttyACM1, or /dev/cu.usbmodem11301",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=115200,
        help="Serial baud rate",
    )
    parser.add_argument(
        "--web-port",
        type=int,
        default=5006,
        help="Panel server port",
    )
    parser.add_argument(
        "--plot-window",
        type=int,
        default=300,
        help="How many recent packets to plot",
    )
    parser.add_argument(
        "--max-rows",
        type=int,
        default=5000,
        help="How many parsed packets to keep in memory",
    )
    parser.add_argument(
        "--update-ms",
        type=int,
        default=1000,
        help="Dashboard update interval in milliseconds",
    )
    parser.add_argument(
        "--verbose-serial",
        action="store_true",
        help="Print every parsed RX packet to the terminal",
    )

    parser.add_argument(
        "--tdma-slot-ms",
        type=int,
        default=1000,
        help="TDMA slot width in milliseconds",
    )
    parser.add_argument(
        "--tdma-num-slots",
        type=int,
        default=8,
        help="Number of slots per TDMA frame",
    )
    parser.add_argument(
        "--tdma-epoch-ms",
        type=int,
        default=None,
        help=(
            "TDMA epoch in milliseconds. If omitted, the first usable packet clock "
            "becomes the epoch."
        ),
    )
    parser.add_argument(
        "--tdma-clock-source",
        choices=["auto", "session_time", "sniffer"],
        default="auto",
        help=(
            "Clock used for TDMA slot classification. "
            "auto prefers session_time_ms and falls back to sniffer millis."
        ),
    )
    parser.add_argument(
        "--tdma-node-offset",
        type=int,
        default=1,
        help=(
            "Expected node id for slot 0 when no explicit slot map is provided. "
            "Example: 1 means slot 0 -> node 1, slot 1 -> node 2."
        ),
    )
    parser.add_argument(
        "--tdma-slot-node-map",
        default="",
        help=(
            "Optional explicit slot-to-node map, e.g. '0:1,1:2,2:5,3:7'. "
            "Overrides --tdma-node-offset."
        ),
    )
    parser.add_argument(
        "--tdma-heatmap-bucket",
        choices=["frame", "slot"],
        default="frame",
        help=(
            "Heatmap time bucket size. 'frame' is usually best for TDMA overview; "
            "'slot' gives finer resolution."
        ),
    )

    args = parser.parse_args()

    try:
        tdma_slot_node_map = parse_slot_node_map(args.tdma_slot_node_map)
    except ValueError as e:
        raise SystemExit(str(e))

    store = PacketStore(max_rows=args.max_rows)
    stop_event = threading.Event()

    reader_thread = threading.Thread(
        target=serial_reader_loop,
        args=(args.serial_port, args.baud, store, stop_event, args.verbose_serial),
        daemon=True,
    )
    reader_thread.start()

    def app_factory():
        return build_dashboard(
            store=store,
            plot_window=args.plot_window,
            update_ms=args.update_ms,
            tdma_slot_ms=args.tdma_slot_ms,
            tdma_num_slots=args.tdma_num_slots,
            tdma_epoch_ms=args.tdma_epoch_ms,
            tdma_clock_source=args.tdma_clock_source,
            tdma_node_offset=args.tdma_node_offset,
            tdma_slot_node_map=tdma_slot_node_map,
            tdma_heatmap_bucket=args.tdma_heatmap_bucket,
        )

    try:
        pn.serve(
            app_factory,
            title="SmartFires LoRa Dashboard",
            port=args.web_port,
            show=True,
            autoreload=False,
        )
    finally:
        stop_event.set()


if __name__ == "__main__":
    main()
