#!/usr/bin/env python3

import argparse
import subprocess
from pathlib import Path

import numpy as np
import pandas as pd

try:
    import plotly.graph_objects as go
    from plotly.subplots import make_subplots
except ImportError as exc:
    raise SystemExit(
        "Missing dependency: plotly. Install with: python3 -m pip install plotly pandas"
    ) from exc


DEFAULT_RSYNC_SOURCE = "jetson-field:/mnt/nvme_drive/data/"
DEFAULT_RSYNC_DEST = "/run/media/kyle/External/SmartFires/"

# Keep the old project colors where they make sense, but allow extra nodes/metrics.
COLOR_BY_METRIC_AND_NODE = {
    "wind_mps": {"1": "#2ca02c", "2": "#98df8a", "3": "#006d2c"},
    "temp_c": {"1": "#d62728", "2": "#ff9896", "3": "#a50f15"},
    "humidity_pct": {"1": "#1f77b4", "2": "#aec7e8", "3": "#08519c"},
    "pm1_0_ug_m3": {"1": "#9467bd", "2": "#c5b0d5", "3": "#54278f"},
    "pm2_5_ug_m3": {"1": "#ff7f0e", "2": "#ffbb78", "3": "#d94801"},
    "pm4_0_ug_m3": {"1": "#8c564b", "2": "#c49c94", "3": "#67000d"},
    "pm10_ug_m3": {"1": "#17becf", "2": "#9edae5", "3": "#006d77"},
    "battery_mv": {"1": "#636363", "2": "#969696", "3": "#252525"},
    "battery_pct": {"1": "#31a354", "2": "#74c476", "3": "#006d2c"},
    "rssi": {"1": "#756bb1", "2": "#9e9ac8", "3": "#54278f"},
    "heading_true_deg": {"1": "#3182bd", "2": "#6baed6", "3": "#08519c"},
    "location_corrected_heading": {"1": "#e6550d", "2": "#fd8d3c", "3": "#a63603"},
    "jetson_wind_mps": {"1": "#238b45", "2": "#41ab5d", "3": "#005a32"},
    "jetson_wind_dir_deg": {"1": "#756bb1", "2": "#9e9ac8", "3": "#54278f"},
    "retx_total": {"1": "#e377c2", "2": "#f7b6d2", "3": "#c51b8a"},
    "fail_total": {"1": "#7f7f7f", "2": "#c7c7c7", "3": "#252525"},
}

FALLBACK_COLORS = [
    "#1f77b4",
    "#ff7f0e",
    "#2ca02c",
    "#d62728",
    "#9467bd",
    "#8c564b",
    "#e377c2",
    "#7f7f7f",
    "#bcbd22",
    "#17becf",
]

METRIC_LABELS = {
    "wind_mps": "Wind speed",
    "temp_c": "Temperature",
    "humidity_pct": "Humidity",
    "pm1_0_ug_m3": "PM1.0",
    "pm2_5_ug_m3": "PM2.5",
    "pm4_0_ug_m3": "PM4.0",
    "pm10_ug_m3": "PM10",
    "battery_mv": "Battery voltage",
    "battery_pct": "Battery charge",
    "rssi": "RSSI",
    "heading_true_deg": "Heading true",
    "location_corrected_heading": "Location-corrected heading",
    "jetson_wind_mps": "Jetson wind speed",
    "jetson_wind_dir_deg": "Jetson wind direction",
    "retx_total": "Retransmits",
    "fail_total": "Failures",
}

NUMERIC_COLUMNS = [
    "node_id",
    "seq",
    "session_time_ms",
    "uptime_ms",
    "sensor_flags",
    "wind_mps",
    "temp_c",
    "humidity_pct",
    "pm1_0_ug_m3",
    "pm2_5_ug_m3",
    "pm4_0_ug_m3",
    "pm10_ug_m3",
    "lat",
    "lon",
    "battery_mv",
    "battery_pct",
    "flags",
    "rssi",
    "heading_true_deg",
    "location_corrected_heading",
    "jetson_wind_mps",
    "jetson_wind_dir_deg",
    "retx_total",
    "fail_total",
]

BOOLEAN_COLUMNS = ["gps_valid", "battery_valid"]


def run_rsync(source: str, dest: str) -> None:
    print(f"Syncing data from {source} to {dest}")
    subprocess.run(["rsync", "-avh", "--progress", source, dest], check=True)
    print("Rsync complete.")


def normalize_node_id(value) -> str:
    if pd.isna(value):
        return "unknown"

    try:
        as_float = float(value)
        if as_float.is_integer():
            return str(int(as_float))
    except (TypeError, ValueError):
        pass

    return str(value).strip()


def parse_bool_column(series: pd.Series) -> pd.Series:
    text = series.astype("string").str.strip().str.lower()
    return text.map(
        {
            "true": True,
            "1": True,
            "yes": True,
            "y": True,
            "false": False,
            "0": False,
            "no": False,
            "n": False,
        }
    )


def parse_timestamps(series: pd.Series) -> pd.Series:
    text = series.astype("string").str.strip()
    text = text.mask(text.eq(""))

    # pandas 2.x can parse mixed ISO strings efficiently. Older pandas may not
    # understand format="mixed", so fall back to per-row parsing.
    try:
        parsed = pd.to_datetime(text, errors="coerce", utc=True, format="mixed")
    except (TypeError, ValueError):
        parsed = text.map(lambda value: pd.to_datetime(value, errors="coerce", utc=True))

    return parsed


def load_data(csv_path: Path) -> pd.DataFrame:
    df = pd.read_csv(
        csv_path,
        low_memory=False,
        na_values=["", " ", "nan", "NaN", "None", "null", "NULL"],
        keep_default_na=True,
    )

    df.columns = [col.strip() for col in df.columns]

    if "timestamp" not in df.columns:
        raise SystemExit("CSV is missing required column: timestamp")

    if "packet_type" not in df.columns:
        df["packet_type"] = "telemetry"

    if "node_id" not in df.columns:
        df["node_id"] = "unknown"

    df["timestamp"] = parse_timestamps(df["timestamp"])
    df["packet_type"] = (
        df["packet_type"].astype("string").str.strip().str.lower().fillna("unknown")
    )

    for col in NUMERIC_COLUMNS:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    for col in BOOLEAN_COLUMNS:
        if col in df.columns:
            df[col] = parse_bool_column(df[col])

    df["node_id_label"] = df["node_id"].map(normalize_node_id)

    if "session_time_ms" in df.columns:
        df["session_time_s"] = df["session_time_ms"] / 1000.0
    else:
        df["session_time_s"] = np.nan

    valid_ts = df["timestamp"].dropna()
    if valid_ts.empty:
        df["packet_time_s"] = df.index.astype(float)
    else:
        t0 = valid_ts.min()
        df["packet_time_s"] = (df["timestamp"] - t0).dt.total_seconds()

    # A single plotting x column that works for mixed packet files. Telemetry
    # samples prefer the device/session clock. Status packets prefer receive time.
    df["auto_time_s"] = df["packet_time_s"]
    telemetry_mask = df["packet_type"].eq("telemetry") & df["session_time_s"].notna()
    df.loc[telemetry_mask, "auto_time_s"] = df.loc[telemetry_mask, "session_time_s"]

    sort_cols = ["node_id_label", "auto_time_s", "seq"]
    existing_sort_cols = [col for col in sort_cols if col in df.columns]
    df = df.sort_values(existing_sort_cols, kind="stable").reset_index(drop=True)

    return df


def choose_x_column(args_x_axis: str) -> str:
    if args_x_axis == "timestamp":
        return "timestamp"
    if args_x_axis == "packet_time":
        return "packet_time_s"
    if args_x_axis == "session_time":
        return "session_time_s"
    return "auto_time_s"


def x_axis_title(x_col: str) -> str:
    return {
        "timestamp": "Timestamp",
        "packet_time_s": "Packet timestamp offset (s)",
        "session_time_s": "Session time (s)",
        "auto_time_s": "Time (s): session for telemetry, packet offset for status",
    }.get(x_col, x_col)


def color_for(metric: str, node_id: str, fallback_index: int) -> str:
    configured = COLOR_BY_METRIC_AND_NODE.get(metric, {}).get(node_id)
    if configured:
        return configured
    return FALLBACK_COLORS[fallback_index % len(FALLBACK_COLORS)]


def downsample(group: pd.DataFrame, max_points: int) -> pd.DataFrame:
    if max_points <= 0 or len(group) <= max_points:
        return group

    positions = np.linspace(0, len(group) - 1, max_points, dtype=int)
    positions = np.unique(positions)
    return group.iloc[positions]


def has_metric_data(df: pd.DataFrame, metrics: list[str]) -> bool:
    available = [metric for metric in metrics if metric in df.columns]
    if not available:
        return False
    return df[available].notna().any().any()


def has_gps_data(df: pd.DataFrame) -> bool:
    if "lat" not in df.columns or "lon" not in df.columns:
        return False
    return df[["lat", "lon"]].dropna().shape[0] > 0


def split_data(df: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame]:
    telemetry_df = df[df["packet_type"].eq("telemetry")].copy()
    status_df = df[df["packet_type"].eq("status")].copy()

    # Backward compatibility with old CSVs that did not have packet_type.
    if telemetry_df.empty:
        telemetry_df = df.copy()

    return telemetry_df, status_df


def build_rows(df: pd.DataFrame, telemetry_df: pd.DataFrame, status_df: pd.DataFrame):
    rows = []

    if has_metric_data(telemetry_df, ["wind_mps"]):
        rows.append(
            {
                "kind": "metrics",
                "title": "Wind speed by node",
                "df": telemetry_df,
                "metrics": ["wind_mps"],
                "ylabel": "Wind speed (m/s)",
                "mode": "markers",
            }
        )

    if has_metric_data(telemetry_df, ["temp_c"]):
        rows.append(
            {
                "kind": "metrics",
                "title": "Temperature by node",
                "df": telemetry_df,
                "metrics": ["temp_c"],
                "ylabel": "Temperature (°C)",
                "mode": "markers",
            }
        )

    if has_metric_data(telemetry_df, ["humidity_pct"]):
        rows.append(
            {
                "kind": "metrics",
                "title": "Humidity by node",
                "df": telemetry_df,
                "metrics": ["humidity_pct"],
                "ylabel": "Humidity (%)",
                "mode": "markers",
            }
        )

    pm_metrics = ["pm1_0_ug_m3", "pm2_5_ug_m3", "pm4_0_ug_m3", "pm10_ug_m3"]
    if has_metric_data(telemetry_df, pm_metrics):
        rows.append(
            {
                "kind": "metrics",
                "title": "Particulate matter by node",
                "df": telemetry_df,
                "metrics": pm_metrics,
                "ylabel": "PM concentration (µg/m³)",
                "mode": "markers",
            }
        )

    if has_metric_data(status_df, ["battery_mv"]):
        rows.append(
            {
                "kind": "metrics",
                "title": "Battery voltage by node",
                "df": status_df,
                "metrics": ["battery_mv"],
                "ylabel": "Battery (mV)",
                "mode": "lines+markers",
            }
        )

    if has_metric_data(status_df, ["battery_pct"]):
        rows.append(
            {
                "kind": "metrics",
                "title": "Battery charge by node",
                "df": status_df,
                "metrics": ["battery_pct"],
                "ylabel": "Battery (%)",
                "mode": "lines+markers",
            }
        )

    if has_metric_data(df, ["rssi"]):
        rows.append(
            {
                "kind": "metrics",
                "title": "RSSI by node",
                "df": df,
                "metrics": ["rssi"],
                "ylabel": "RSSI (dBm)",
                "mode": "markers",
            }
        )

    heading_metrics = ["heading_true_deg", "location_corrected_heading"]
    if has_metric_data(status_df, heading_metrics):
        rows.append(
            {
                "kind": "metrics",
                "title": "Heading by node",
                "df": status_df,
                "metrics": heading_metrics,
                "ylabel": "Heading (deg)",
                "mode": "lines+markers",
            }
        )

    jetson_metrics = ["jetson_wind_mps", "jetson_wind_dir_deg"]
    if has_metric_data(df, jetson_metrics):
        rows.append(
            {
                "kind": "metrics",
                "title": "Jetson wind estimate",
                "df": df,
                "metrics": jetson_metrics,
                "ylabel": "Wind estimate",
                "mode": "markers",
            }
        )

    link_metrics = ["retx_total", "fail_total"]
    if has_metric_data(status_df, link_metrics):
        rows.append(
            {
                "kind": "metrics",
                "title": "Radio link counters by node",
                "df": status_df,
                "metrics": link_metrics,
                "ylabel": "Count",
                "mode": "lines+markers",
            }
        )

    if has_gps_data(df):
        rows.append(
            {
                "kind": "gps",
                "title": "GPS position by node",
                "df": df,
                "ylabel": "Latitude",
            }
        )

    return rows


def add_metric_traces(
    fig: go.Figure,
    row_idx: int,
    frame: pd.DataFrame,
    metrics: list[str],
    x_col: str,
    mode: str,
    max_points_per_trace: int,
) -> None:
    for metric in metrics:
        if metric not in frame.columns:
            continue

        needed_cols = ["node_id_label", x_col, metric]
        plot_df = frame[needed_cols].dropna().sort_values(["node_id_label", x_col])
        if plot_df.empty:
            continue

        for fallback_index, (node_id, group) in enumerate(plot_df.groupby("node_id_label", sort=True)):
            group = downsample(group, max_points_per_trace)
            metric_label = METRIC_LABELS.get(metric, metric)
            trace_name = f"Node {node_id} {metric_label}"
            fig.add_trace(
                go.Scattergl(
                    x=group[x_col],
                    y=group[metric],
                    mode=mode,
                    name=trace_name,
                    marker={
                        "size": 4,
                        "opacity": 0.75,
                        "color": color_for(metric, str(node_id), fallback_index),
                    },
                    line={"width": 1.25, "color": color_for(metric, str(node_id), fallback_index)},
                    hovertemplate=(
                        f"{trace_name}<br>"
                        + "%{x}<br>"
                        + f"{metric}: "
                        + "%{y}<extra></extra>"
                    ),
                ),
                row=row_idx,
                col=1,
            )


def add_gps_traces(
    fig: go.Figure,
    row_idx: int,
    frame: pd.DataFrame,
    max_points_per_trace: int,
) -> None:
    plot_df = frame[["node_id_label", "lat", "lon"]].dropna().sort_values(
        ["node_id_label", "lon", "lat"]
    )

    if plot_df.empty:
        return

    for fallback_index, (node_id, group) in enumerate(plot_df.groupby("node_id_label", sort=True)):
        group = downsample(group, max_points_per_trace)
        fig.add_trace(
            go.Scattergl(
                x=group["lon"],
                y=group["lat"],
                mode="markers+lines",
                name=f"Node {node_id} GPS",
                marker={
                    "size": 5,
                    "opacity": 0.8,
                    "color": FALLBACK_COLORS[fallback_index % len(FALLBACK_COLORS)],
                },
                line={
                    "width": 1,
                    "color": FALLBACK_COLORS[fallback_index % len(FALLBACK_COLORS)],
                },
                hovertemplate="Node %{text}<br>lon: %{x}<br>lat: %{y}<extra></extra>",
                text=[node_id] * len(group),
            ),
            row=row_idx,
            col=1,
        )


def resolve_x_column_for_frame(frame: pd.DataFrame, requested_x_col: str) -> str:
    if requested_x_col in frame.columns and frame[requested_x_col].notna().any():
        return requested_x_col

    # Status packets usually do not have session_time_ms. Fall back cleanly
    # instead of silently producing empty status plots.
    for fallback in ["auto_time_s", "packet_time_s", "timestamp", "session_time_s"]:
        if fallback in frame.columns and frame[fallback].notna().any():
            return fallback

    return requested_x_col


def make_dashboard(
    df: pd.DataFrame,
    x_col: str,
    max_points_per_trace: int,
) -> go.Figure:
    telemetry_df, status_df = split_data(df)
    rows = build_rows(df, telemetry_df, status_df)

    if not rows:
        raise SystemExit("No plottable numeric data found in CSV.")

    fig = make_subplots(
        rows=len(rows),
        cols=1,
        subplot_titles=[row["title"] for row in rows],
        vertical_spacing=min(0.04, 0.35 / max(len(rows), 1)),
    )

    for row_idx, row in enumerate(rows, start=1):
        if row["kind"] == "metrics":
            row_x_col = resolve_x_column_for_frame(row["df"], x_col)
            add_metric_traces(
                fig=fig,
                row_idx=row_idx,
                frame=row["df"],
                metrics=row["metrics"],
                x_col=row_x_col,
                mode=row["mode"],
                max_points_per_trace=max_points_per_trace,
            )
            fig.update_yaxes(title_text=row["ylabel"], row=row_idx, col=1)
            fig.update_xaxes(title_text=x_axis_title(row_x_col), row=row_idx, col=1)
        elif row["kind"] == "gps":
            add_gps_traces(fig, row_idx, row["df"], max_points_per_trace)
            fig.update_yaxes(title_text="Latitude", row=row_idx, col=1)
            fig.update_xaxes(title_text="Longitude", row=row_idx, col=1)

    height = max(420, 280 * len(rows))
    fig.update_layout(
        title="SmartFires Drone Data",
        height=height,
        width=1200,
        template="plotly_white",
        legend={"orientation": "h", "yanchor": "bottom", "y": -0.08, "xanchor": "left", "x": 0},
        margin={"l": 75, "r": 30, "t": 80, "b": 90},
    )

    return fig


def print_summary(df: pd.DataFrame, x_col: str, max_points_per_trace: int) -> None:
    packet_counts = df["packet_type"].value_counts(dropna=False).to_dict()
    nodes = sorted(df["node_id_label"].dropna().unique().tolist())
    print(f"Loaded rows: {len(df)}")
    print(f"Packet types: {packet_counts}")
    print(f"Nodes: {', '.join(nodes)}")
    print(f"X axis: {x_col} ({x_axis_title(x_col)})")
    if max_points_per_trace > 0:
        print(f"Downsampling: max {max_points_per_trace} points per trace")
    else:
        print("Downsampling: disabled")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Plot SmartFires mixed status/telemetry CSV data as a fast Plotly HTML dashboard."
    )

    parser.add_argument("csv", type=Path, help="Path to SmartFires CSV file")

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("smartfires_plots.html"),
        help="Output HTML file path",
    )

    parser.add_argument(
        "--x-axis",
        choices=["auto", "timestamp", "packet_time", "session_time"],
        default="auto",
        help=(
            "X axis to use. auto uses session_time_ms for telemetry rows and packet timestamp "
            "offset for status rows. Default: auto"
        ),
    )

    parser.add_argument(
        "--max-points-per-trace",
        type=int,
        default=5000,
        help="Downsample each trace to at most this many points. Use 0 to disable. Default: 5000",
    )

    parser.add_argument(
        "--plotlyjs",
        choices=["inline", "cdn"],
        default="inline",
        help="Use inline for a standalone HTML file, or cdn for a smaller file that needs internet. Default: inline",
    )

    parser.add_argument(
        "--rsync",
        action="store_true",
        help="Pull latest data from the Jetson before plotting",
    )

    parser.add_argument(
        "--rsync-source",
        default=DEFAULT_RSYNC_SOURCE,
        help=f"Rsync source. Default: {DEFAULT_RSYNC_SOURCE}",
    )

    parser.add_argument(
        "--rsync-dest",
        default=DEFAULT_RSYNC_DEST,
        help=f"Rsync destination. Default: {DEFAULT_RSYNC_DEST}",
    )

    args = parser.parse_args()

    if args.rsync:
        run_rsync(args.rsync_source, args.rsync_dest)

    df = load_data(args.csv)
    x_col = choose_x_column(args.x_axis)

    if x_col not in df.columns:
        raise SystemExit(f"Internal error: requested x column does not exist: {x_col}")

    print_summary(df, x_col, args.max_points_per_trace)

    fig = make_dashboard(
        df=df,
        x_col=x_col,
        max_points_per_trace=args.max_points_per_trace,
    )

    include_plotlyjs = True if args.plotlyjs == "inline" else "cdn"
    fig.write_html(args.output, include_plotlyjs=include_plotlyjs, full_html=True)

    print(f"Saved plots to: {args.output}")


if __name__ == "__main__":
    main()
