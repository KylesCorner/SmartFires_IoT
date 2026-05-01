#!/usr/bin/env python3

import argparse
import json
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

SMARTFIRES_MAGIC = 0xA5

# Python owns packet naming/filtering. Firmware only emits raw payload bytes.
PACKET_TYPE_NAMES = {
    0x01: "FULL_STATE",
    0x02: "HEARTBEAT",
    0x03: "TIME_SYNC",
    0x04: "BUNDLE",
    0x05: "STATUS",
    0x06: "AWAKEN",
    0x07: "ACK_SUMMARY",
}


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
            "payload_hex",
            "payload_len",
            "magic",
            "is_smartfires",
            "pkt_type_id",
            "pkt_type_name",
            "node",
            "seq",
            "session_time_ms",
            "tdma_clock_ms",
            "tdma_frame",
            "tdma_slot",
            "tdma_slot_phase_ms",
            "tdma_slot_progress",
            "expected_node",
            "slot_match",
            "slot_guard_zone",
            "raw_json",
        ]
    )


class PacketStore:
    def __init__(self, max_rows=5000, max_raw=300):
        self.rows = deque(maxlen=max_rows)
        self.raw_lines = deque(maxlen=max_raw)
        self.lock = threading.Lock()

    def add_packet(self, row):
        with self.lock:
            self.rows.append(row)

    def add_raw(self, line, parsed_event=None):
        with self.lock:
            self.raw_lines.append(
                {
                    "time": pd.Timestamp.now(),
                    "event": parsed_event,
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
                return pd.DataFrame(columns=["time", "event", "line"])
            return pd.DataFrame(list(self.raw_lines))


def to_int(value, default=None):
    if value is None:
        return default

    try:
        if isinstance(value, bool):
            return int(value)
        return int(value)
    except (TypeError, ValueError):
        return default


def parse_payload_hex(payload_hex):
    if not payload_hex:
        return b""

    text = str(payload_hex).strip().replace(" ", "")

    if len(text) % 2 != 0:
        return b""

    try:
        return bytes.fromhex(text)
    except ValueError:
        return b""


def packet_type_name(pkt_type_id):
    if pkt_type_id is None:
        return "NO_TYPE"

    return PACKET_TYPE_NAMES.get(
        int(pkt_type_id),
        f"UNKNOWN_0x{int(pkt_type_id):02X}",
    )


def decode_smartfires_payload(payload_hex):
    raw = parse_payload_hex(payload_hex)

    decoded = {
        "payload_len": len(raw),
        "magic": None,
        "is_smartfires": False,
        "pkt_type_id": None,
        "pkt_type_name": "TOO_SHORT",
        "node": None,
        "seq": None,
        "session_time_ms": None,
    }

    if len(raw) < 4:
        return decoded

    magic = raw[0]
    pkt_type_id = raw[1]
    node = raw[2]
    seq = raw[3]

    decoded["magic"] = magic
    decoded["is_smartfires"] = magic == SMARTFIRES_MAGIC
    decoded["pkt_type_id"] = pkt_type_id
    decoded["pkt_type_name"] = packet_type_name(pkt_type_id)
    decoded["node"] = node
    decoded["seq"] = seq

    # Current SmartFires refactor/only-feather assumption:
    # FULL_STATE and BUNDLE begin with FullStatePayload after the 4-byte header.
    # FullStatePayload begins with uint32_t session_time little-endian.
    if decoded["is_smartfires"] and pkt_type_id in (0x01, 0x04) and len(raw) >= 8:
        decoded["session_time_ms"] = int.from_bytes(
            raw[4:8],
            byteorder="little",
            signed=False,
        )

    return decoded


def parse_json_line(line):
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return None

    if not isinstance(obj, dict):
        return None

    return obj


def parse_rx_json(line):
    obj = parse_json_line(line)

    if obj is None:
        return None

    if obj.get("event") != "rx":
        return None

    payload_hex = str(obj.get("payload_hex", "")).upper()
    decoded = decode_smartfires_payload(payload_hex)

    row = {
        "wall_time": pd.Timestamp.now(),
        "count": to_int(obj.get("count"), 0),
        "t_ms": to_int(obj.get("t_ms"), 0),
        "dt_ms": to_int(obj.get("dt_ms"), 0),
        "length": to_int(obj.get("len"), decoded["payload_len"]),
        "rssi": to_int(obj.get("rssi"), 0),
        "snr": to_int(obj.get("snr")),
        "rh_to": to_int(obj.get("rh_to"), 0),
        "rh_from": to_int(obj.get("rh_from"), 0),
        "rh_id": to_int(obj.get("rh_id"), 0),
        "rh_flags": to_int(obj.get("rh_flags"), 0),
        "payload_hex": payload_hex,
        "raw_json": line,
    }

    row.update(decoded)
    return row


def parse_slot_node_map(raw_map):
    """
    Parses strings like:
      0:1,1:2,2:5,3:7

    Meaning:
      TDMA slot 0 expects node 1
      TDMA slot 1 expects node 2
      TDMA slot 2 expects node 5
      TDMA slot 3 expects node 7
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


def expected_node_for_slot(slot, node_offset, slot_node_map):
    if slot is None or pd.isna(slot):
        return None

    slot_int = int(slot)

    if slot_node_map:
        return slot_node_map.get(slot_int)

    return slot_int + int(node_offset)


def compute_tdma_fields(row, tdma_epoch_ms, slot_ms, num_slots, clock_source, guard_ms):
    """
    clock_source:
      auto         prefer SmartFires session_time_ms, fall back to sniffer t_ms
      session_time use SmartFires session_time_ms only
      sniffer      use sniffer-local millis only
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
            "slot_guard_zone": None,
        }

    slot_clock_ms = int(packet_clock_ms) - int(tdma_epoch_ms)

    if slot_clock_ms < 0:
        return {
            "tdma_clock_ms": int(packet_clock_ms),
            "tdma_frame": None,
            "tdma_slot": None,
            "tdma_slot_phase_ms": None,
            "tdma_slot_progress": None,
            "slot_guard_zone": None,
        }

    frame_ms = int(slot_ms) * int(num_slots)
    frame = slot_clock_ms // frame_ms
    frame_phase_ms = slot_clock_ms % frame_ms
    slot = frame_phase_ms // int(slot_ms)
    slot_phase_ms = frame_phase_ms % int(slot_ms)

    in_start_guard = guard_ms > 0 and slot_phase_ms < guard_ms
    in_end_guard = guard_ms > 0 and slot_phase_ms >= (slot_ms - guard_ms)

    if in_start_guard:
        guard_zone = "start"
    elif in_end_guard:
        guard_zone = "end"
    else:
        guard_zone = "ok"

    return {
        "tdma_clock_ms": int(packet_clock_ms),
        "tdma_frame": int(frame),
        "tdma_slot": int(slot),
        "tdma_slot_phase_ms": int(slot_phase_ms),
        "tdma_slot_progress": float(slot_phase_ms) / float(slot_ms),
        "slot_guard_zone": guard_zone,
    }


def serial_reader_loop(port, baud, store, stop_event, verbose=False):
    while not stop_event.is_set():
        try:
            with serial.Serial(port, baudrate=baud, timeout=1) as ser:
                print(f"Connected to {port} @ {baud}")

                while not stop_event.is_set():
                    raw = ser.readline()
                    if not raw:
                        continue

                    line = raw.decode("utf-8", errors="replace").strip()
                    if not line:
                        continue

                    parsed_obj = parse_json_line(line)
                    parsed_event = parsed_obj.get("event") if parsed_obj else "non_json"
                    store.add_raw(line, parsed_event=parsed_event)

                    row = parse_rx_json(line)
                    if row is None:
                        continue

                    if verbose:
                        print(
                            f"RX count={row['count']} "
                            f"type={row['pkt_type_name']} "
                            f"node={row['node']} "
                            f"seq={row['seq']} "
                            f"rssi={row['rssi']}"
                        )

                    store.add_packet(row)

        except serial.SerialException as e:
            print(f"Serial error: {e}")
            time.sleep(2)
        except Exception as e:
            print(f"Reader error: {e}")
            time.sleep(2)


def source_template():
    return {
        "wall_time": [],
        "time_text": [],
        "rssi": [],
        "dt_ms": [],
        "rh_from": [],
        "rh_to": [],
        "node": [],
        "pkt_type_name": [],
        "pkt_type_id": [],
        "seq": [],
        "length": [],
        "tdma_frame": [],
        "tdma_slot": [],
        "tdma_slot_phase_ms": [],
        "expected_node": [],
        "slot_match": [],
        "slot_guard_zone": [],
    }


def dataframe_for_plot(df, plot_window):
    if df.empty:
        return pd.DataFrame(source_template())

    plot_df = df.sort_values("wall_time").tail(plot_window).copy()
    plot_df["time_text"] = plot_df["wall_time"].dt.strftime("%H:%M:%S")
    plot_df["pkt_type_name"] = plot_df["pkt_type_name"].fillna("NO_TYPE")
    plot_df["pkt_type_id"] = plot_df["pkt_type_id"].fillna(-1).astype(int)
    plot_df["seq"] = plot_df["seq"].fillna(-1).astype(int)
    plot_df["node"] = plot_df["node"].fillna(-1).astype(int)
    plot_df["tdma_frame"] = plot_df["tdma_frame"].fillna(-1).astype(int)
    plot_df["tdma_slot"] = plot_df["tdma_slot"].fillna(-1).astype(int)
    plot_df["tdma_slot_phase_ms"] = plot_df["tdma_slot_phase_ms"].fillna(-1).astype(int)
    plot_df["expected_node"] = plot_df["expected_node"].fillna(-1).astype(int)
    plot_df["slot_match"] = plot_df["slot_match"].map(
        lambda v: bool(v) if pd.notna(v) else False
    )
    plot_df["slot_guard_zone"] = plot_df["slot_guard_zone"].fillna("unknown")

    return plot_df


def make_rssi_plot(source):
    p = figure(
        title="RSSI over time",
        x_axis_type="datetime",
        height=320,
        sizing_mode="stretch_width",
        tools="pan,wheel_zoom,box_zoom,reset,save",
    )
    p.scatter(x="wall_time", y="rssi", source=source, size=7)
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
                ("type", "@pkt_type_name"),
                ("type id", "@pkt_type_id"),
                ("seq", "@seq"),
                ("len", "@length"),
                ("tdma frame", "@tdma_frame"),
                ("tdma slot", "@tdma_slot"),
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
    p.line(x="wall_time", y="dt_ms", source=source, line_width=2)
    p.scatter(x="wall_time", y="dt_ms", source=source, size=5)
    p.xaxis.axis_label = "Time"
    p.yaxis.axis_label = "dt_ms"
    p.add_tools(
        HoverTool(
            tooltips=[
                ("time", "@time_text"),
                ("dt_ms", "@dt_ms"),
                ("node", "@node"),
                ("type", "@pkt_type_name"),
                ("seq", "@seq"),
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
    p.scatter(x="wall_time", y="tdma_slot_phase_ms", source=source, size=7)
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
                ("guard", "@slot_guard_zone"),
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
    p.vbar(x="category", top="packets", width=0.8, source=source)
    p.xaxis.axis_label = x_label
    p.yaxis.axis_label = y_label
    p.xaxis.major_label_orientation = 0.8
    p.add_tools(
        HoverTool(tooltips=[("category", "@category"), ("packets", "@packets")])
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
                ("guard packets", "@guard_packets"),
                ("avg RSSI", "@avg_rssi"),
            ]
        )
    )
    return p


def build_dashboard(
    store,
    plot_window,
    update_ms,
    tdma_slot_ms,
    tdma_num_slots,
    tdma_guard_ms,
    tdma_epoch_ms,
    tdma_clock_source,
    tdma_node_offset,
    tdma_slot_node_map,
    tdma_heatmap_bucket,
):
    tdma_state = {"epoch_ms": tdma_epoch_ms}

    total_packets = pn.indicators.Number(
        name="Total Packets", value=0, format="{value}"
    )
    last_60s = pn.indicators.Number(name="Packets Last 60s", value=0, format="{value}")
    unique_nodes = pn.indicators.Number(name="Unique Nodes", value=0, format="{value}")
    avg_rssi = pn.indicators.Number(
        name="Avg RSSI Last 100", value=0, format="{value:.1f}"
    )
    bad_slot_packets = pn.indicators.Number(
        name="Bad Slot Packets", value=0, format="{value}"
    )
    guard_zone_packets = pn.indicators.Number(
        name="Guard-Zone Packets", value=0, format="{value}"
    )

    latest_packet = pn.widgets.TextAreaInput(
        name="Latest RX JSON",
        value="Waiting for packets...",
        disabled=True,
        height=115,
        sizing_mode="stretch_width",
    )

    decode_summary = pn.pane.Markdown("**Packet summary:** waiting...", height=110)
    tdma_summary = pn.pane.Markdown("**TDMA:** waiting...", height=130)

    recent_packets = pn.widgets.Tabulator(
        pd.DataFrame(),
        disabled=True,
        pagination="local",
        page_size=15,
        sizing_mode="stretch_width",
        height=460,
        sorters=[{"field": "wall_time", "dir": "desc"}],
    )

    tdma_activity = pn.widgets.Tabulator(
        pd.DataFrame(),
        disabled=True,
        pagination="local",
        page_size=20,
        sizing_mode="stretch_width",
        height=460,
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
        height=460,
    )

    plot_source = ColumnDataSource(data=source_template())
    source_bar_source = ColumnDataSource(data={"category": [], "packets": []})
    type_bar_source = ColumnDataSource(data={"category": [], "packets": []})
    slot_bar_source = ColumnDataSource(data={"category": [], "packets": []})
    heatmap_source = ColumnDataSource(
        data={
            "bucket_time": [],
            "time_text": [],
            "slot_label": [],
            "packets": [],
            "nodes": [],
            "bad_slot_packets": [],
            "guard_packets": [],
            "avg_rssi": [],
            "bucket_width_ms": [],
        }
    )

    rssi_plot = make_rssi_plot(plot_source)
    dt_plot = make_dt_plot(plot_source)
    slot_phase_plot = make_slot_phase_plot(plot_source)
    source_bar = make_bar_plot(
        "Packets by RadioHead source", "RH source", "Packets", source_bar_source
    )
    type_bar = make_bar_plot(
        "Packets by SmartFires packet type", "Packet type", "Packets", type_bar_source
    )
    slot_bar = make_bar_plot(
        "Packets by TDMA slot", "TDMA slot", "Packets", slot_bar_source
    )
    heatmap = make_tdma_heatmap(heatmap_source, tdma_num_slots, high_packets_per_cell=5)

    plots = pn.Column(
        pn.pane.Bokeh(heatmap, sizing_mode="stretch_width"),
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
        ("Recent Raw Serial JSON", recent_raw),
        dynamic=False,
        sizing_mode="stretch_width",
    )

    last_rendered_signature = {"value": None}

    def choose_epoch(df):
        if tdma_state["epoch_ms"] is not None or df.empty:
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

        tdma_rows = [
            compute_tdma_fields(
                row,
                tdma_epoch_ms=tdma_state["epoch_ms"],
                slot_ms=tdma_slot_ms,
                num_slots=tdma_num_slots,
                clock_source=tdma_clock_source,
                guard_ms=tdma_guard_ms,
            )
            for _, row in df.iterrows()
        ]

        tdma_df = pd.DataFrame(tdma_rows, index=df.index)
        for col in tdma_df.columns:
            df[col] = tdma_df[col]

        df["expected_node"] = df["tdma_slot"].apply(
            lambda s: expected_node_for_slot(s, tdma_node_offset, tdma_slot_node_map)
        )

        def slot_matches(row):
            node = row.get("node")
            expected = row.get("expected_node")

            if node is None or expected is None:
                return None
            if pd.isna(node) or pd.isna(expected):
                return None

            return int(node) == int(expected)

        df["slot_match"] = df.apply(slot_matches, axis=1)
        return df

    def join_nodes(series):
        vals = []
        for value in series.dropna():
            vals.append(str(int(value)))
        return ",".join(sorted(set(vals), key=lambda s: int(s))) if vals else ""

    def make_tdma_activity_df(df):
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
                    "guard_packets",
                    "ok_packets",
                ]
            )

        activity = (
            working.groupby(["tdma_frame", "tdma_slot"])
            .agg(
                packets=("count", "count"),
                nodes=("node", join_nodes),
                expected_node=("expected_node", "first"),
                avg_rssi=("rssi", "mean"),
                min_phase_ms=("tdma_slot_phase_ms", "min"),
                max_phase_ms=("tdma_slot_phase_ms", "max"),
                bad_slot_packets=("slot_match", lambda x: int((x == False).sum())),
                guard_packets=("slot_guard_zone", lambda x: int((x != "ok").sum())),
                ok_packets=("slot_guard_zone", lambda x: int((x == "ok").sum())),
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

    def update_heatmap(df):
        working = df.dropna(subset=["tdma_slot"]).copy()
        if working.empty:
            heatmap_source.data = {
                "bucket_time": [],
                "time_text": [],
                "slot_label": [],
                "packets": [],
                "nodes": [],
                "bad_slot_packets": [],
                "guard_packets": [],
                "avg_rssi": [],
                "bucket_width_ms": [],
            }
            return

        frame_ms = tdma_slot_ms * tdma_num_slots
        bucket_ms = tdma_slot_ms if tdma_heatmap_bucket == "slot" else frame_ms

        working = working.sort_values("wall_time").tail(plot_window).copy()
        first_time = working["wall_time"].min()

        working["bucket_index"] = (
            (working["wall_time"] - first_time).dt.total_seconds() * 1000.0 / bucket_ms
        ).astype(int)

        working["bucket_time"] = first_time + pd.to_timedelta(
            working["bucket_index"] * bucket_ms,
            unit="ms",
        )

        heat_df = (
            working.assign(slot_label=working["tdma_slot"].astype(int).astype(str))
            .groupby(["bucket_time", "slot_label"])
            .agg(
                packets=("count", "count"),
                nodes=("node", join_nodes),
                bad_slot_packets=("slot_match", lambda x: int((x == False).sum())),
                guard_packets=("slot_guard_zone", lambda x: int((x != "ok").sum())),
                avg_rssi=("rssi", "mean"),
            )
            .reset_index()
        )

        heat_df["time_text"] = heat_df["bucket_time"].dt.strftime("%H:%M:%S")
        heat_df["avg_rssi"] = heat_df["avg_rssi"].round(1)
        heat_df["bucket_width_ms"] = bucket_ms

        heatmap_source.data = {
            "bucket_time": heat_df["bucket_time"].tolist(),
            "time_text": heat_df["time_text"].tolist(),
            "slot_label": heat_df["slot_label"].tolist(),
            "packets": heat_df["packets"].tolist(),
            "nodes": heat_df["nodes"].tolist(),
            "bad_slot_packets": heat_df["bad_slot_packets"].tolist(),
            "guard_packets": heat_df["guard_packets"].tolist(),
            "avg_rssi": heat_df["avg_rssi"].tolist(),
            "bucket_width_ms": heat_df["bucket_width_ms"].tolist(),
        }

    def update_bars(df, plot_df):
        per_source = (
            plot_df.assign(source=plot_df["rh_from"].astype(str))
            .groupby("source")
            .size()
            .reset_index(name="packets")
            .sort_values("source")
        )
        source_bar.x_range.factors = per_source["source"].tolist()
        source_bar_source.data = {
            "category": per_source["source"].tolist(),
            "packets": per_source["packets"].tolist(),
        }

        per_type = (
            plot_df.assign(type_label=plot_df["pkt_type_name"].fillna("NO_TYPE"))
            .groupby("type_label")
            .size()
            .reset_index(name="packets")
            .sort_values("packets", ascending=False)
        )
        type_bar.x_range.factors = per_type["type_label"].tolist()
        type_bar_source.data = {
            "category": per_type["type_label"].tolist(),
            "packets": per_type["packets"].tolist(),
        }

        per_slot = (
            df.dropna(subset=["tdma_slot"])
            .assign(slot_label=lambda x: x["tdma_slot"].astype(int).astype(str))
            .groupby("slot_label")
            .size()
            .reset_index(name="packets")
        )

        if not per_slot.empty:
            per_slot["slot_sort"] = per_slot["slot_label"].astype(int)
            per_slot = per_slot.sort_values("slot_sort")

        slot_bar.x_range.factors = per_slot["slot_label"].tolist()
        slot_bar_source.data = {
            "category": per_slot["slot_label"].tolist(),
            "packets": per_slot["packets"].tolist(),
        }

    def update():
        df = store.dataframe()
        raw_df = store.raw_dataframe()

        if df.empty:
            total_packets.value = 0
            last_60s.value = 0
            unique_nodes.value = 0
            avg_rssi.value = 0
            bad_slot_packets.value = 0
            guard_zone_packets.value = 0
            latest_packet.value = "Waiting for packets..."
            decode_summary.object = "**Packet summary:** waiting..."
            tdma_summary.object = "**TDMA:** waiting..."
            plot_source.data = source_template()
            source_bar.x_range.factors = []
            type_bar.x_range.factors = []
            slot_bar.x_range.factors = []
            source_bar_source.data = {"category": [], "packets": []}
            type_bar_source.data = {"category": [], "packets": []}
            slot_bar_source.data = {"category": [], "packets": []}
            update_heatmap(df)
            recent_packets.value = pd.DataFrame()
            tdma_activity.value = pd.DataFrame()
        else:
            df = df.sort_values("wall_time").copy()
            df = attach_tdma_fields(df)

            latest_count = int(df.iloc[-1]["count"])
            latest_raw_time = str(raw_df.iloc[-1]["time"]) if not raw_df.empty else None
            signature = (latest_count, latest_raw_time)

            if last_rendered_signature["value"] == signature:
                return

            last_rendered_signature["value"] = signature

            now = pd.Timestamp.now()
            recent_60 = df[df["wall_time"] >= now - pd.Timedelta(seconds=60)]

            total_packets.value = int(len(df))
            last_60s.value = int(len(recent_60))
            unique_nodes.value = int(df["node"].dropna().nunique())
            avg_rssi.value = float(df["rssi"].tail(100).mean())
            bad_slot_packets.value = int((df["slot_match"] == False).sum())
            guard_zone_packets.value = int((df["slot_guard_zone"] != "ok").sum())

            latest = df.iloc[-1]
            latest_packet.value = str(latest["raw_json"])

            type_counts = df["pkt_type_name"].value_counts(dropna=False).to_dict()
            smartfires_count = int((df["is_smartfires"] == True).sum())
            non_smartfires_count = int((df["is_smartfires"] == False).sum())

            decode_summary.object = (
                f"**SmartFires packets:** `{smartfires_count}`  \n"
                f"**Non-SmartFires/too-short packets:** `{non_smartfires_count}`  \n"
                f"**Packet type counts:** `{type_counts}`"
            )

            tdma_summary.object = (
                f"**TDMA config**  \n"
                f"- slot width: `{tdma_slot_ms} ms`  \n"
                f"- guard width: `{tdma_guard_ms} ms`  \n"
                f"- slots/frame: `{tdma_num_slots}`  \n"
                f"- frame width: `{tdma_slot_ms * tdma_num_slots} ms`  \n"
                f"- clock source: `{tdma_clock_source}`  \n"
                f"- epoch ms: `{tdma_state['epoch_ms']}`  \n\n"
                f"**Latest packet TDMA**  \n"
                f"- type: `{latest.get('pkt_type_name')}` / `{latest.get('pkt_type_id')}`  \n"
                f"- node: `{latest.get('node')}` seq: `{latest.get('seq')}`  \n"
                f"- frame: `{latest.get('tdma_frame')}` slot: `{latest.get('tdma_slot')}`  \n"
                f"- phase: `{latest.get('tdma_slot_phase_ms')} ms` guard: `{latest.get('slot_guard_zone')}`  \n"
                f"- expected node: `{latest.get('expected_node')}` slot match: `{latest.get('slot_match')}`"
            )

            plot_df = dataframe_for_plot(df, plot_window)
            plot_source.data = {
                key: plot_df[key].tolist()
                for key in source_template().keys()
                if key in plot_df.columns
            }

            update_bars(df, plot_df)
            update_heatmap(df)
            tdma_activity.value = make_tdma_activity_df(df)

            display_cols = [
                "wall_time",
                "count",
                "rh_from",
                "rh_to",
                "is_smartfires",
                "magic",
                "pkt_type_name",
                "pkt_type_id",
                "node",
                "seq",
                "length",
                "payload_len",
                "rssi",
                "snr",
                "dt_ms",
                "session_time_ms",
                "tdma_clock_ms",
                "tdma_frame",
                "tdma_slot",
                "tdma_slot_phase_ms",
                "slot_guard_zone",
                "expected_node",
                "slot_match",
                "payload_hex",
            ]

            display_df = (
                df[[c for c in display_cols if c in df.columns]].tail(250).copy()
            )
            display_df = display_df.sort_values("wall_time", ascending=False)
            display_df["wall_time"] = display_df["wall_time"].dt.strftime("%H:%M:%S")
            recent_packets.value = display_df

        if raw_df.empty:
            recent_raw.value = pd.DataFrame(columns=["time", "event", "line"])
        else:
            raw_display = raw_df.sort_values("time", ascending=False).head(150).copy()
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
        max_width=1200,
        margin=(10, 20),
    )

    update()
    pn.state.add_periodic_callback(update, period=update_ms, start=True)
    return dashboard


def main():
    parser = argparse.ArgumentParser(
        description="Live dashboard for NDJSON SmartFires LoRa sniffer output"
    )
    parser.add_argument(
        "--local-only",
        action="store_true",
        help=(
            "Serve only on localhost for debugging with hardware attached to this machine. "
            "Use this when the Feather sniffer is plugged directly into the laptop/Jetson "
            "running the dashboard."
        ),
    )
    parser.add_argument("--serial-port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--web-port", type=int, default=5006)
    parser.add_argument("--address", default="0.0.0.0")
    parser.add_argument(
        "--allow-websocket-origin",
        action="append",
        default=[],
        help=(
            "Allowed Bokeh websocket origin, e.g. 192.168.4.1:5006. "
            "May be passed multiple times."
        ),
    )
    parser.add_argument("--plot-window", type=int, default=300)
    parser.add_argument("--max-rows", type=int, default=5000)
    parser.add_argument("--update-ms", type=int, default=1000)
    parser.add_argument("--verbose-serial", action="store_true")

    parser.add_argument("--tdma-slot-ms", type=int, default=900)
    parser.add_argument("--tdma-num-slots", type=int, default=4)
    parser.add_argument("--tdma-guard-ms", type=int, default=20)
    parser.add_argument("--tdma-epoch-ms", type=int, default=None)
    parser.add_argument(
        "--tdma-clock-source",
        choices=["auto", "session_time", "sniffer"],
        default="auto",
    )
    parser.add_argument("--tdma-node-offset", type=int, default=1)
    parser.add_argument(
        "--tdma-slot-node-map",
        default="",
        help="Optional explicit slot map, e.g. '0:1,1:2,2:3,3:4'.",
    )
    parser.add_argument(
        "--tdma-heatmap-bucket",
        choices=["frame", "slot"],
        default="frame",
    )

    args = parser.parse_args()

    try:
        slot_node_map = parse_slot_node_map(args.tdma_slot_node_map)
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
            tdma_guard_ms=args.tdma_guard_ms,
            tdma_epoch_ms=args.tdma_epoch_ms,
            tdma_clock_source=args.tdma_clock_source,
            tdma_node_offset=args.tdma_node_offset,
            tdma_slot_node_map=slot_node_map,
            tdma_heatmap_bucket=args.tdma_heatmap_bucket,
        )

    default_origins = [
        f"localhost:{args.web_port}",
        f"127.0.0.1:{args.web_port}",
        f"192.168.4.1:{args.web_port}",
    ]
    websocket_origins = list(
        dict.fromkeys(default_origins + args.allow_websocket_origin)
    )

    try:
        # pn.serve(
        #     app_factory,
        #     title="SmartFires LoRa Dashboard",
        #     address=args.address,
        #     port=args.web_port,
        #     show=False,
        #     autoreload=False,
        #     websocket_origin=websocket_origins,
        # )
        if args.local_only:
            serve_address = "127.0.0.1"
            websocket_origins = [
                f"localhost:{args.web_port}",
                f"127.0.0.1:{args.web_port}",
            ]
            show_browser = True
        else:
            serve_address = "0.0.0.0"
            websocket_origins = [
                f"localhost:{args.web_port}",
                f"127.0.0.1:{args.web_port}",
                f"192.168.4.1:{args.web_port}",
                f"10.8.184.94:{args.web_port}",
            ]
            show_browser = False

        pn.serve(
            app_factory,
            title="SmartFires LoRa Dashboard",
            address=serve_address,
            port=args.web_port,
            show=show_browser,
            autoreload=False,
            websocket_origin=websocket_origins,
        )
    finally:
        stop_event.set()


if __name__ == "__main__":
    main()
