#!/usr/bin/env python3
"""Generate editable SVG plots for the September 15 SmartFires burn.

The script creates four standalone figures and one combined poster figure:

* temperature_humidity.svg
* pm25.svg
* wind_intensity.svg
* environmental_deviation.svg
* synchronized_sensor_response.svg

All plotted traces, event markers, and labels remain vector objects. Matplotlib
is configured to keep text as SVG text so labels can be edited in Inkscape.

Example:
    python generate_burn_plots.py
    python generate_burn_plots.py --pm-scale linear --aggregation-seconds 30
    python generate_burn_plots.py --nodes 2 3 4 5
    python generate_burn_plots.py --event-labels major --also-png
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from datetime import date, datetime, time
from pathlib import Path
from typing import Iterable, Sequence
from zoneinfo import ZoneInfo

import matplotlib as mpl
mpl.use("Agg")
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd
from matplotlib.axes import Axes
from matplotlib.figure import Figure
from matplotlib.lines import Line2D
from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter


SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE_DIR = Path(__file__).resolve().parents[2]
DEFAULT_BURN_DIR = WORKSPACE_DIR / "analysis and scripts" / "data" / "2026-09-15_last_burn"
DEFAULT_TELEMETRY = DEFAULT_BURN_DIR / "wireless_nodes" / "telemetry.csv"
DEFAULT_BASE_ENVIRONMENT = DEFAULT_BURN_DIR / "jetson" / "bme688.csv"
DEFAULT_ANEMOMETER = DEFAULT_BURN_DIR / "jetson" / "anemometer.csv"
DEFAULT_TIMELINE = DEFAULT_BURN_DIR / "BURN_TIMELINE.txt"
DEFAULT_OUTPUT_DIR = SCRIPT_DIR / "generated_plots"

AVAILABLE_NODE_IDS = (2, 3, 4, 5)
DEFAULT_NODE_IDS = (4, 5)
NODE_COLORS = {
    2: "#2D6A8A",  # poster blue
    3: "#8D5A97",  # poster purple
    4: "#B33A3A",  # temperature / generic node red
    5: "#E67E22",  # temperature / generic node orange
}
HUMIDITY_COLORS = {
    2: "#5E88A3",
    3: "#7A9A70",
    4: "#2D6A8A",  # poster blue
    5: "#66885D",  # poster green
}
BASE_COLOR = "#000000"
TEXT_COLOR = "#17324D"
MUTED_TEXT = "#647683"
GRID_COLOR = "#DCE3E8"
AXIS_COLOR = "#758793"
PANEL_COLOR = "#FBFCFD"
DATA_LINE_WIDTH = 0.6
BASE_LINE_WIDTH = 0.72

EVENT_STYLES = {
    "run": {"color": "#17324D", "linestyle": "-", "linewidth": 0.85, "alpha": 0.72},
    "ignition": {"color": "#D96D12", "linestyle": "-", "linewidth": 1.2, "alpha": 0.95},
    "fuel": {"color": "#8A2637", "linestyle": (0, (2.2, 1.6)), "linewidth": 0.8, "alpha": 0.75},
    "last_wood": {"color": "#8A2637", "linestyle": "-", "linewidth": 1.3, "alpha": 0.95},
    "fire": {"color": "#D96D12", "linestyle": (0, (1.0, 1.5)), "linewidth": 0.8, "alpha": 0.78},
    "equipment": {"color": "#7D8C97", "linestyle": (0, (1.0, 1.5)), "linewidth": 0.7, "alpha": 0.72},
    "environment": {"color": "#66885D", "linestyle": (0, (1.0, 1.5)), "linewidth": 0.75, "alpha": 0.78},
}


@dataclass(frozen=True)
class PlotSettings:
    timezone: ZoneInfo
    start: pd.Timestamp
    end: pd.Timestamp
    node_ids: tuple[int, ...]
    aggregation_seconds: int
    max_connected_gap_seconds: int
    wind_rolling_window: int
    pm_max_ug_m3: float
    pm_scale: str
    event_labels: str


@dataclass(frozen=True)
class BurnEvent:
    timestamp: pd.Timestamp
    label: str
    category: str
    major: bool = False


def slugify(value: str) -> str:
    """Return a short SVG-safe identifier."""
    return re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")


def configure_matplotlib() -> None:
    """Apply poster typography and preserve editable SVG text."""
    mpl.rcParams.update(
        {
            "font.family": "DejaVu Sans",
            "font.size": 9.5,
            "axes.labelcolor": TEXT_COLOR,
            "axes.titlecolor": TEXT_COLOR,
            "axes.edgecolor": AXIS_COLOR,
            "axes.linewidth": 0.8,
            "xtick.color": MUTED_TEXT,
            "ytick.color": MUTED_TEXT,
            "text.color": TEXT_COLOR,
            "svg.fonttype": "none",
            "svg.hashsalt": "smartfires-poster-2026-09-15",
            "path.simplify": True,
            "path.simplify_threshold": 0.05,
            "savefig.facecolor": "white",
        }
    )


def parse_clock(value: str) -> time:
    try:
        return datetime.strptime(value, "%H:%M").time()
    except ValueError as exc:
        raise argparse.ArgumentTypeError("time must use 24-hour HH:MM format") from exc


def local_timestamp(day: date, clock: time, timezone: ZoneInfo) -> pd.Timestamp:
    return pd.Timestamp(datetime.combine(day, clock, tzinfo=timezone))


def classify_event(label: str) -> tuple[str, bool]:
    lower = label.lower()
    if lower == "ignition":
        return "ignition", True
    if lower in {"start recording", "end"}:
        return "run", True
    if "add " in lower or "pine needles" in lower:
        return "fuel", lower in {"add wood", "add 2x wood"}
    if "stir fire" in lower:
        return "fire", False
    if "sun" in lower:
        return "environment", False
    return "equipment", False


def load_events(
    timeline_path: Path,
    burn_date: date,
    timezone: ZoneInfo,
) -> list[BurnEvent]:
    """Parse the human-maintained burn timeline.

    Timeline clock values are interpreted as PM because the source file uses
    12-hour times without AM/PM and this burn occurred from 4:10–8:30 PM.
    """
    pattern = re.compile(r"^(\d{1,2}):(\d{2})\s+-\s+(.+?)\s*$")
    raw_events: list[tuple[pd.Timestamp, str, str, bool]] = []
    for line_number, line in enumerate(timeline_path.read_text(encoding="utf-8").splitlines(), 1):
        match = pattern.match(line.strip())
        if not match:
            continue
        hour = int(match.group(1))
        minute = int(match.group(2))
        if not 1 <= hour <= 12 or not 0 <= minute <= 59:
            raise ValueError(f"Invalid timeline time on line {line_number}: {line!r}")
        if hour != 12:
            hour += 12
        label = match.group(3)
        category, major = classify_event(label)
        timestamp = local_timestamp(burn_date, time(hour, minute), timezone)
        raw_events.append((timestamp, label, category, major))

    if not raw_events:
        raise ValueError(f"No timeline entries found in {timeline_path}")

    wood_indices = [i for i, (_, label, _, _) in enumerate(raw_events) if "wood" in label.lower()]
    if wood_indices:
        final_wood_index = wood_indices[-1]
        timestamp, label, _, _ = raw_events[final_wood_index]
        raw_events[final_wood_index] = (timestamp, f"{label} (final)", "last_wood", True)

    return [BurnEvent(*event) for event in raw_events]


def load_node_telemetry(path: Path, node_ids: Sequence[int] = DEFAULT_NODE_IDS) -> pd.DataFrame:
    """Load, filter, and de-duplicate node sensor samples."""
    frame = pd.read_csv(path, low_memory=False)
    frame = frame.loc[frame["packet_type"].eq("telemetry")].copy()
    frame["node_id"] = pd.to_numeric(frame["node_id"], errors="coerce")
    frame = frame.loc[frame["node_id"].isin(node_ids)].copy()
    frame["node_id"] = frame["node_id"].astype(int)
    frame["timestamp"] = pd.to_datetime(frame["timestamp"], utc=True, errors="coerce")
    frame = frame.dropna(subset=["timestamp"])

    numeric_columns = [
        "session_time_ms",
        "rssi",
        "sensor_flags",
        "delta_flags",
        "temp_c",
        "humidity_pct",
        "pm1_0_ug_m3",
        "pm2_5_ug_m3",
        "pm4_0_ug_m3",
        "pm10_ug_m3",
        "wind_mps",
    ]
    for column in numeric_columns:
        frame[column] = pd.to_numeric(frame[column], errors="coerce")

    frame = (
        frame.sort_values("rssi", ascending=False, na_position="last")
        .drop_duplicates(subset=["node_id", "session_time_ms"], keep="first")
        .sort_values(["node_id", "timestamp"])
        .reset_index(drop=True)
    )
    return frame


def filter_pm25_readings(
    frame: pd.DataFrame,
    max_ug_m3: float,
) -> tuple[pd.DataFrame, dict[str, int]]:
    """Mask PM2.5 samples that fail transport, range, or nesting checks."""
    filtered = frame.copy()
    pm25 = filtered["pm2_5_ug_m3"]
    pm10 = filtered["pm10_ug_m3"]
    delta_flags = filtered["delta_flags"].fillna(0).astype("int64")

    negative = pm25 < 0
    over_range = pm25 > max_ug_m3
    delta_clamped = (delta_flags & 0x10) != 0
    inconsistent = pm25.notna() & pm10.notna() & (pm25 > pm10 + 0.2)
    rejected = negative | over_range | delta_clamped | inconsistent

    stats = {
        "total": int(rejected.sum()),
        "negative": int(negative.sum()),
        "over_range": int(over_range.sum()),
        "delta_clamped": int(delta_clamped.sum()),
        "inconsistent": int(inconsistent.sum()),
    }
    filtered.loc[rejected, "pm2_5_ug_m3"] = float("nan")
    return filtered, stats


def load_base_environment(path: Path) -> pd.DataFrame:
    """Load the Jetson BME688 stream used as the environmental reference."""
    frame = pd.read_csv(path)
    frame["timestamp"] = pd.to_datetime(frame["host_epoch_ms"], unit="ms", utc=True)
    for column in ("temperature_c", "humidity_percent"):
        frame[column] = pd.to_numeric(frame[column], errors="coerce")
    return frame.sort_values("timestamp").reset_index(drop=True)


def load_anemometer(path: Path) -> pd.DataFrame:
    """Load the Jetson anemometer stream used on the wind panel."""
    frame = pd.read_csv(path)
    frame["timestamp"] = pd.to_datetime(frame["host_epoch_ms"], unit="ms", utc=True)
    frame["speed_mps"] = pd.to_numeric(frame["speed_mps"], errors="coerce")
    return frame.sort_values("timestamp").reset_index(drop=True)


def aggregate_nodes(frame: pd.DataFrame, settings: PlotSettings) -> dict[int, pd.DataFrame]:
    """Aggregate each node with medians and leave empty bins as NaN gaps."""
    rule = f"{settings.aggregation_seconds}s"
    columns = ["temp_c", "humidity_pct", "pm2_5_ug_m3", "wind_mps"]
    result: dict[int, pd.DataFrame] = {}
    for node_id in settings.node_ids:
        node = frame.loc[frame["node_id"].eq(node_id), ["timestamp", *columns]].copy()
        node["timestamp"] = node["timestamp"].dt.tz_convert(settings.timezone)
        node = node.set_index("timestamp").sort_index()
        aggregated = node[columns].resample(rule).median()
        result[node_id] = aggregated.loc[
            (aggregated.index >= settings.start) & (aggregated.index <= settings.end)
        ]
    return result


def aggregate_base(frame: pd.DataFrame, settings: PlotSettings) -> pd.DataFrame:
    rule = f"{settings.aggregation_seconds}s"
    base = frame[["timestamp", "temperature_c", "humidity_percent"]].copy()
    base["timestamp"] = base["timestamp"].dt.tz_convert(settings.timezone)
    base = base.set_index("timestamp").sort_index().resample(rule).median()
    return base.loc[(base.index >= settings.start) & (base.index <= settings.end)]


def aggregate_anemometer(frame: pd.DataFrame, settings: PlotSettings) -> pd.DataFrame:
    rule = f"{settings.aggregation_seconds}s"
    anemometer = frame[["timestamp", "speed_mps"]].copy()
    anemometer["timestamp"] = anemometer["timestamp"].dt.tz_convert(settings.timezone)
    anemometer = anemometer.set_index("timestamp").sort_index().resample(rule).median()
    return anemometer.loc[
        (anemometer.index >= settings.start) & (anemometer.index <= settings.end)
    ]


def rolling_point_mean(series: pd.Series, window: int) -> pd.Series:
    """Apply a centered rolling mean to recorded points without filling gaps."""
    recorded = series.dropna().sort_index()
    return recorded.rolling(window=window, center=True, min_periods=1).mean()


def connect_short_gaps(series: pd.Series, max_gap_seconds: int) -> pd.Series:
    """Connect recorded values across routine pauses but retain long blank gaps."""
    recorded = series.dropna().sort_index()
    if recorded.empty:
        return recorded

    timestamps: list[pd.Timestamp] = [recorded.index[0]]
    values: list[float] = [float(recorded.iloc[0])]
    for previous_time, current_time, value in zip(
        recorded.index[:-1],
        recorded.index[1:],
        recorded.iloc[1:],
    ):
        gap_seconds = (current_time - previous_time).total_seconds()
        if gap_seconds > max_gap_seconds:
            timestamps.append(previous_time + (current_time - previous_time) / 2)
            values.append(float("nan"))
        timestamps.append(current_time)
        values.append(float(value))
    return pd.Series(values, index=pd.DatetimeIndex(timestamps), name=series.name)


def style_axis(ax: Axes, ylabel: str | None = None) -> None:
    ax.set_facecolor(PANEL_COLOR)
    ax.grid(axis="y", color=GRID_COLOR, linewidth=0.75, alpha=0.9)
    ax.grid(axis="x", visible=False)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.tick_params(axis="both", labelsize=8.5, length=3, width=0.7)
    if ylabel:
        ax.set_ylabel(ylabel, fontweight="bold", labelpad=8)


def apply_time_axis(ax: Axes, settings: PlotSettings, show_labels: bool = True) -> None:
    ax.set_xlim(settings.start, settings.end)
    ax.xaxis.set_major_locator(mdates.MinuteLocator(byminute=[0, 30], tz=settings.timezone))

    def format_time(value: float, _position: int) -> str:
        stamp = mdates.num2date(value, tz=settings.timezone)
        return stamp.strftime("%I:%M %p").lstrip("0")

    ax.xaxis.set_major_formatter(FuncFormatter(format_time))
    ax.tick_params(axis="x", labelbottom=show_labels)
    if show_labels:
        ax.set_xlabel("Local time (MDT)", fontweight="bold", labelpad=8)
    else:
        ax.set_xlabel("")


def node_legend_handles(node_ids: Sequence[int], include_base: bool = False) -> list[Line2D]:
    handles = [
        Line2D(
            [0],
            [0],
            color=NODE_COLORS[node_id],
            linewidth=1.2,
            linestyle="-",
            label=f"Node {node_id}",
        )
        for node_id in node_ids
    ]
    if include_base:
        handles.append(
            Line2D(
                [0],
                [0],
                color=BASE_COLOR,
                linewidth=1.25,
                linestyle="-",
                label="Base station",
            )
        )
    return handles


def add_figure_legend(
    fig: Figure,
    node_ids: Sequence[int],
    include_base: bool,
    include_variables: bool = False,
) -> None:
    node_legend = fig.legend(
        handles=node_legend_handles(node_ids, include_base),
        loc="upper center",
        bbox_to_anchor=(0.5, 0.935),
        ncol=len(node_ids) + int(include_base),
        frameon=False,
        fontsize=8.5,
        handlelength=2.6,
        columnspacing=1.3,
    )
    node_legend.set_gid("legend-nodes")
    if include_variables:
        temperature_handles = [
            Line2D(
                [0],
                [0],
                color=NODE_COLORS[node_id],
                linewidth=1.2,
                linestyle="-",
                label=f"Node {node_id} temperature",
            )
            for node_id in node_ids
        ]
        if include_base:
            temperature_handles.append(
                Line2D(
                    [0],
                    [0],
                    color=BASE_COLOR,
                    linewidth=1.25,
                    linestyle="-",
                    label="Base temperature",
                )
            )
        node_legend.remove()
        temperature_legend = fig.legend(
            handles=temperature_handles,
            loc="upper center",
            bbox_to_anchor=(0.5, 0.935),
            ncol=len(temperature_handles),
            frameon=False,
            fontsize=8.5,
            handlelength=2.6,
            columnspacing=1.3,
        )
        temperature_legend.set_gid("legend-temperature")

        humidity_handles = [
            Line2D(
                [0],
                [0],
                color=HUMIDITY_COLORS[node_id],
                linewidth=1.2,
                linestyle="-",
                label=f"Node {node_id} humidity",
            )
            for node_id in node_ids
        ]
        if include_base:
            humidity_handles.append(
                Line2D(
                    [0],
                    [0],
                    color=BASE_COLOR,
                    linewidth=1.1,
                    linestyle=(0, (2.0, 1.5)),
                    label="Base humidity",
                )
            )
        humidity_legend = fig.legend(
            handles=humidity_handles,
            loc="upper center",
            bbox_to_anchor=(0.5, 0.895),
            ncol=len(humidity_handles),
            frameon=False,
            fontsize=8.2,
            handlelength=2.8,
            columnspacing=1.5,
        )
        humidity_legend.set_gid("legend-humidity")


def add_wind_figure_legend(fig: Figure, node_ids: Sequence[int]) -> None:
    handles = node_legend_handles(node_ids, include_base=False)
    handles.append(
        Line2D(
            [0],
            [0],
            color=BASE_COLOR,
            linewidth=1.25,
            linestyle="-",
            label="Jetson anemometer",
        )
    )
    legend = fig.legend(
        handles=handles,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.935),
        ncol=len(handles),
        frameon=False,
        fontsize=8.5,
        handlelength=2.6,
        columnspacing=1.3,
    )
    legend.set_gid("legend-wind-sources")


def add_method_note(fig: Figure, settings: PlotSettings, extra: str = "") -> None:
    note = (
        f"Lines connect {settings.aggregation_seconds}-second medians; "
        f"gaps longer than {settings.max_connected_gap_seconds} seconds remain blank."
    )
    if extra:
        note = f"{note} {extra}"
    text = fig.text(
        0.5,
        0.018,
        note,
        ha="center",
        va="bottom",
        fontsize=7.5,
        color=MUTED_TEXT,
    )
    text.set_gid("figure-method-note")


def add_events(
    axes: Sequence[Axes],
    events: Iterable[BurnEvent],
    settings: PlotSettings,
    label_axis: Axes,
    id_prefix: str,
    label_rotation: float = 90.0,
    label_stagger_levels: int = 2,
) -> None:
    """Draw identical event markers and optional editable labels."""
    visible_events = [event for event in events if settings.start <= event.timestamp <= settings.end]
    for event_index, event in enumerate(visible_events):
        style = EVENT_STYLES[event.category]
        event_slug = slugify(event.label)
        for axis_index, ax in enumerate(axes):
            line = ax.axvline(event.timestamp, zorder=1.5, **style)
            line.set_gid(f"{id_prefix}-event-line-{event_index:02d}-{axis_index}-{event_slug}")

        should_label = settings.event_labels == "all" or (
            settings.event_labels == "major" and event.major
        )
        if should_label:
            annotation = label_axis.annotate(
                event.label,
                xy=(event.timestamp, 1.0),
                xycoords=("data", "axes fraction"),
                xytext=(
                    2 if label_rotation != 90.0 else 0,
                    7 + 9 * (event_index % label_stagger_levels),
                ),
                textcoords="offset points",
                ha="left",
                va="bottom",
                rotation=label_rotation,
                rotation_mode="anchor",
                fontsize=6.2,
                color=style["color"],
                clip_on=False,
                zorder=5,
            )
            annotation.set_gid(f"{id_prefix}-event-label-{event_index:02d}-{event_slug}")


def draw_temperature_humidity(
    ax: Axes,
    nodes: dict[int, pd.DataFrame],
    base: pd.DataFrame,
    settings: PlotSettings,
    prefix: str,
) -> Axes:
    """Draw temperature and RH on one panel and return the RH twin axis."""
    style_axis(ax, "Temperature (°C)")
    humidity_ax = ax.twinx()
    humidity_ax.spines["top"].set_visible(False)
    humidity_ax.spines["left"].set_visible(False)
    humidity_ax.spines["right"].set_color(AXIS_COLOR)
    humidity_ax.tick_params(axis="y", colors=MUTED_TEXT, labelsize=8.5, length=3, width=0.7)
    humidity_ax.set_ylabel("Relative humidity (%)", fontweight="bold", color=TEXT_COLOR, labelpad=8)
    humidity_ax.patch.set_visible(False)

    for node_id, frame in nodes.items():
        temperature = connect_short_gaps(
            frame["temp_c"], settings.max_connected_gap_seconds
        )
        temperature_line, = ax.plot(
            temperature.index,
            temperature,
            color=NODE_COLORS[node_id],
            linewidth=DATA_LINE_WIDTH,
            linestyle="-",
            alpha=0.88,
            zorder=3,
        )
        temperature_line.set_gid(f"{prefix}-node-{node_id}-temperature")
        humidity = connect_short_gaps(
            frame["humidity_pct"], settings.max_connected_gap_seconds
        )
        humidity_line, = humidity_ax.plot(
            humidity.index,
            humidity,
            color=HUMIDITY_COLORS[node_id],
            linewidth=DATA_LINE_WIDTH,
            linestyle="-",
            alpha=0.85,
            zorder=2.8,
        )
        humidity_line.set_gid(f"{prefix}-node-{node_id}-humidity")

    base_temperature_values = connect_short_gaps(
        base["temperature_c"], settings.max_connected_gap_seconds
    )
    base_temperature, = ax.plot(
        base_temperature_values.index,
        base_temperature_values,
        color=BASE_COLOR,
        linewidth=BASE_LINE_WIDTH,
        linestyle="-",
        alpha=0.86,
        zorder=3.3,
    )
    base_temperature.set_gid(f"{prefix}-base-temperature")
    base_humidity_values = connect_short_gaps(
        base["humidity_percent"], settings.max_connected_gap_seconds
    )
    base_humidity, = humidity_ax.plot(
        base_humidity_values.index,
        base_humidity_values,
        color=BASE_COLOR,
        linewidth=BASE_LINE_WIDTH,
        linestyle=(0, (2.0, 1.5)),
        alpha=0.72,
        zorder=3.1,
    )
    base_humidity.set_gid(f"{prefix}-base-humidity")
    apply_time_axis(ax, settings)
    return humidity_ax


def draw_pm25(
    ax: Axes,
    nodes: dict[int, pd.DataFrame],
    settings: PlotSettings,
    prefix: str,
) -> None:
    scale_label = "log scale" if settings.pm_scale == "log" else "linear scale"
    style_axis(ax, f"PM2.5 (µg/m³; {scale_label})")
    for node_id, frame in nodes.items():
        values = frame["pm2_5_ug_m3"].where(frame["pm2_5_ug_m3"] > 0)
        values = connect_short_gaps(values, settings.max_connected_gap_seconds)
        line, = ax.plot(
            values.index,
            values,
            color=NODE_COLORS[node_id],
            linewidth=DATA_LINE_WIDTH,
            linestyle="-",
            alpha=0.88,
            zorder=3,
        )
        line.set_gid(f"{prefix}-node-{node_id}-pm25")
    ax.set_yscale(settings.pm_scale)
    if settings.pm_scale == "log":
        ax.yaxis.set_major_locator(LogLocator(base=10, numticks=8))
        ax.yaxis.set_minor_locator(LogLocator(base=10, subs=(2, 5), numticks=16))
        ax.yaxis.set_minor_formatter(NullFormatter())
        ax.set_ylim(bottom=0.8)
    else:
        ax.set_ylim(bottom=0)
    apply_time_axis(ax, settings)


def draw_wind(
    ax: Axes,
    nodes: dict[int, pd.DataFrame],
    anemometer: pd.DataFrame,
    settings: PlotSettings,
    prefix: str,
) -> Axes:
    style_axis(ax, "Node wind intensity (m/s)")
    for node_id, frame in nodes.items():
        values = rolling_point_mean(frame["wind_mps"], settings.wind_rolling_window)
        values = connect_short_gaps(values, settings.max_connected_gap_seconds)
        line, = ax.plot(
            values.index,
            values,
            color=NODE_COLORS[node_id],
            linewidth=DATA_LINE_WIDTH,
            linestyle="-",
            alpha=0.88,
            zorder=3,
        )
        line.set_gid(f"{prefix}-node-{node_id}-wind")
    ax.set_ylim(bottom=0)

    anemometer_ax = ax.twinx()
    anemometer_ax.spines["top"].set_visible(False)
    anemometer_ax.spines["left"].set_visible(False)
    anemometer_ax.spines["right"].set_color(BASE_COLOR)
    anemometer_ax.tick_params(
        axis="y", colors=BASE_COLOR, labelsize=8.5, length=3, width=0.7
    )
    anemometer_ax.set_ylabel(
        "Anemometer wind speed (m/s)",
        fontweight="bold",
        color=BASE_COLOR,
        labelpad=8,
    )
    anemometer_ax.patch.set_visible(False)
    anemometer_values = rolling_point_mean(
        anemometer["speed_mps"], settings.wind_rolling_window
    )
    anemometer_values = connect_short_gaps(
        anemometer_values, settings.max_connected_gap_seconds
    )
    anemometer_line, = anemometer_ax.plot(
        anemometer_values.index,
        anemometer_values,
        color=BASE_COLOR,
        linewidth=0.25,
        linestyle="-",
        alpha=0.9,
        zorder=3.2,
    )
    anemometer_line.set_gid(f"{prefix}-jetson-anemometer")
    anemometer_ax.set_ylim(bottom=0)
    apply_time_axis(ax, settings)
    return anemometer_ax


def calculate_deviations(
    nodes: dict[int, pd.DataFrame], base: pd.DataFrame
) -> dict[int, pd.DataFrame]:
    deviations: dict[int, pd.DataFrame] = {}
    base_reference = base[["temperature_c", "humidity_percent"]]
    for node_id, node in nodes.items():
        aligned = node[["temp_c", "humidity_pct"]].join(base_reference, how="left")
        deviations[node_id] = pd.DataFrame(
            {
                "delta_temperature_c": aligned["temp_c"] - aligned["temperature_c"],
                "delta_humidity_pct": aligned["humidity_pct"] - aligned["humidity_percent"],
            },
            index=aligned.index,
        )
    return deviations


def save_figure(fig: Figure, output_path: Path, also_png: bool) -> list[Path]:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    metadata = {"Creator": "SmartFires generate_burn_plots.py", "Date": None}
    fig.savefig(output_path, format="svg", metadata=metadata)
    written = [output_path]
    if also_png:
        png_path = output_path.with_suffix(".png")
        fig.savefig(png_path, dpi=180, metadata={"Software": "SmartFires generate_burn_plots.py"})
        written.append(png_path)
    plt.close(fig)
    return written


def plot_temperature_humidity(
    nodes: dict[int, pd.DataFrame],
    base: pd.DataFrame,
    events: Sequence[BurnEvent],
    settings: PlotSettings,
    output_dir: Path,
    also_png: bool,
) -> list[Path]:
    fig, ax = plt.subplots(figsize=(13.36, 5.2))
    fig.suptitle("Temperature and Relative Humidity", fontsize=15, fontweight="bold", y=0.985)
    draw_temperature_humidity(ax, nodes, base, settings, "temperature-humidity")
    add_events([ax], events, settings, ax, "temperature-humidity")
    add_figure_legend(fig, settings.node_ids, include_base=True, include_variables=True)
    add_method_note(fig, settings)
    fig.subplots_adjust(left=0.075, right=0.925, bottom=0.15, top=0.67)
    return save_figure(fig, output_dir / "temperature_humidity.svg", also_png)


def plot_pm25(
    nodes: dict[int, pd.DataFrame],
    events: Sequence[BurnEvent],
    settings: PlotSettings,
    output_dir: Path,
    also_png: bool,
) -> list[Path]:
    fig, ax = plt.subplots(figsize=(13.36, 4.8))
    scale_name = "log scale" if settings.pm_scale == "log" else "linear scale"
    fig.suptitle(f"PM2.5 Concentration ({scale_name})", fontsize=15, fontweight="bold", y=0.985)
    draw_pm25(ax, nodes, settings, "pm25")
    add_events([ax], events, settings, ax, "pm25")
    add_figure_legend(fig, settings.node_ids, include_base=False)
    add_method_note(
        fig,
        settings,
        f"PM2.5 QC masks values above {settings.pm_max_ug_m3:g} µg/m³, "
        "PM2.5-clamped deltas, and PM2.5 > PM10 inconsistencies.",
    )
    fig.subplots_adjust(left=0.075, right=0.975, bottom=0.16, top=0.68)
    return save_figure(fig, output_dir / "pm25.svg", also_png)


def plot_wind_intensity(
    nodes: dict[int, pd.DataFrame],
    anemometer: pd.DataFrame,
    events: Sequence[BurnEvent],
    settings: PlotSettings,
    output_dir: Path,
    also_png: bool,
) -> list[Path]:
    fig, ax = plt.subplots(figsize=(13.36, 4.8))
    fig.suptitle("Node and Anemometer Wind Intensity", fontsize=15, fontweight="bold", y=0.985)
    draw_wind(ax, nodes, anemometer, settings, "wind")
    add_events([ax], events, settings, ax, "wind")
    add_wind_figure_legend(fig, settings.node_ids)
    add_method_note(
        fig,
        settings,
        f"Wind series use a centered {settings.wind_rolling_window}-point rolling mean; "
        "node values are shown as reported.",
    )
    fig.subplots_adjust(left=0.075, right=0.925, bottom=0.16, top=0.68)
    return save_figure(fig, output_dir / "wind_intensity.svg", also_png)


def plot_environmental_deviation(
    nodes: dict[int, pd.DataFrame],
    base: pd.DataFrame,
    events: Sequence[BurnEvent],
    settings: PlotSettings,
    output_dir: Path,
    also_png: bool,
) -> list[Path]:
    deviations = calculate_deviations(nodes, base)
    fig, axes = plt.subplots(2, 1, figsize=(13.36, 7.2), sharex=True)
    fig.suptitle("Node Deviation from Base-Station Environment", fontsize=15, fontweight="bold", y=0.988)
    labels_and_columns = [
        ("Temperature difference (°C)", "delta_temperature_c", "temperature"),
        ("RH difference (percentage points)", "delta_humidity_pct", "humidity"),
    ]
    for ax, (ylabel, column, variable_name) in zip(axes, labels_and_columns):
        style_axis(ax, ylabel)
        zero_line = ax.axhline(0, color=BASE_COLOR, linewidth=0.75, alpha=0.8, zorder=2)
        zero_line.set_gid(f"deviation-{variable_name}-zero-reference")
        for node_id, frame in deviations.items():
            values = connect_short_gaps(frame[column], settings.max_connected_gap_seconds)
            line, = ax.plot(
                values.index,
                values,
                color=(
                    NODE_COLORS[node_id]
                    if variable_name == "temperature"
                    else HUMIDITY_COLORS[node_id]
                ),
                linewidth=DATA_LINE_WIDTH,
                linestyle="-",
                alpha=0.88,
                zorder=3,
            )
            line.set_gid(f"deviation-node-{node_id}-{variable_name}")
    apply_time_axis(axes[0], settings, show_labels=False)
    apply_time_axis(axes[1], settings, show_labels=True)
    add_events(axes, events, settings, axes[0], "deviation")
    add_figure_legend(fig, settings.node_ids, include_base=False, include_variables=True)
    add_method_note(fig, settings, "Differences use the aligned Jetson BME688 base-station record.")
    fig.subplots_adjust(left=0.09, right=0.975, bottom=0.11, top=0.76, hspace=0.16)
    return save_figure(fig, output_dir / "environmental_deviation.svg", also_png)


def plot_synchronized_stack(
    nodes: dict[int, pd.DataFrame],
    base: pd.DataFrame,
    anemometer: pd.DataFrame,
    events: Sequence[BurnEvent],
    settings: PlotSettings,
    output_dir: Path,
    also_png: bool,
) -> list[Path]:
    fig, axes = plt.subplots(
        3,
        1,
        figsize=(13.36, 10.4),
        sharex=True,
        gridspec_kw={"height_ratios": [2.4, 1.0, 1.0]},
    )
    fig.suptitle("Synchronized Sensor Response", fontsize=16, fontweight="bold", y=0.992)
    draw_temperature_humidity(axes[0], nodes, base, settings, "stack-temperature-humidity")
    draw_wind(axes[1], nodes, anemometer, settings, "stack-wind")
    draw_pm25(axes[2], nodes, settings, "stack-pm25")
    apply_time_axis(axes[0], settings, show_labels=False)
    apply_time_axis(axes[1], settings, show_labels=False)
    apply_time_axis(axes[2], settings, show_labels=True)
    add_events(
        axes,
        events,
        settings,
        axes[0],
        "stack",
        label_rotation=45.0,
        label_stagger_levels=4,
    )
    add_figure_legend(fig, settings.node_ids, include_base=True, include_variables=True)
    add_method_note(
        fig,
        settings,
        f"Wind series use a centered {settings.wind_rolling_window}-point rolling mean; "
        f"node values are shown as reported. PM2.5 QC masks values above "
        f"{settings.pm_max_ug_m3:g} µg/m³ and flagged/inconsistent samples.",
    )
    fig.subplots_adjust(left=0.077, right=0.925, bottom=0.082, top=0.765, hspace=0.16)
    return save_figure(fig, output_dir / "synchronized_sensor_response.svg", also_png)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate editable SVG plots for the September 15 SmartFires burn.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--telemetry", type=Path, default=DEFAULT_TELEMETRY)
    parser.add_argument("--base-environment", type=Path, default=DEFAULT_BASE_ENVIRONMENT)
    parser.add_argument("--anemometer", type=Path, default=DEFAULT_ANEMOMETER)
    parser.add_argument("--timeline", type=Path, default=DEFAULT_TIMELINE)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--burn-date", type=date.fromisoformat, default=date(2026, 9, 15))
    parser.add_argument("--timezone", default="America/Denver")
    parser.add_argument(
        "--nodes",
        type=int,
        nargs="+",
        default=list(DEFAULT_NODE_IDS),
        metavar="NODE_ID",
        help="node IDs to include in every figure",
    )
    parser.add_argument("--start", type=parse_clock, default=time(16, 10), help="24-hour local HH:MM")
    parser.add_argument("--end", type=parse_clock, default=time(20, 30), help="24-hour local HH:MM")
    parser.add_argument(
        "--aggregation-seconds",
        type=int,
        default=15,
        help="median aggregation window; empty windows remain gaps",
    )
    parser.add_argument(
        "--max-connected-gap-seconds",
        type=int,
        default=120,
        help="connect normal sampling pauses up to this duration; preserve longer gaps",
    )
    parser.add_argument(
        "--wind-rolling-window",
        type=int,
        default=5,
        help="number of plotted wind samples in the centered rolling mean",
    )
    parser.add_argument(
        "--pm-max-ug-m3",
        type=float,
        default=1000.0,
        help="maximum in-range SPS30 PM2.5 value; larger readings are masked",
    )
    parser.add_argument("--pm-scale", choices=("log", "linear"), default="log")
    parser.add_argument(
        "--event-labels",
        choices=("all", "major", "none"),
        default="all",
        help="which event labels to draw; all event lines are always retained",
    )
    parser.add_argument("--also-png", action="store_true", help="also write raster previews")
    return parser


def validate_args(args: argparse.Namespace) -> None:
    if args.aggregation_seconds <= 0:
        raise ValueError("--aggregation-seconds must be greater than zero")
    if args.max_connected_gap_seconds <= 0:
        raise ValueError("--max-connected-gap-seconds must be greater than zero")
    if args.wind_rolling_window <= 0:
        raise ValueError("--wind-rolling-window must be greater than zero")
    if args.pm_max_ug_m3 <= 0:
        raise ValueError("--pm-max-ug-m3 must be greater than zero")
    if len(set(args.nodes)) != len(args.nodes):
        raise ValueError("--nodes must not contain duplicates")
    unsupported_nodes = sorted(set(args.nodes) - set(AVAILABLE_NODE_IDS))
    if unsupported_nodes:
        raise ValueError(f"unsupported node IDs: {unsupported_nodes}")
    if not args.nodes:
        raise ValueError("--nodes requires at least one node ID")
    for path in (args.telemetry, args.base_environment, args.anemometer, args.timeline):
        if not path.is_file():
            raise FileNotFoundError(path)


def main() -> int:
    args = build_parser().parse_args()
    validate_args(args)
    timezone = ZoneInfo(args.timezone)
    settings = PlotSettings(
        timezone=timezone,
        start=local_timestamp(args.burn_date, args.start, timezone),
        end=local_timestamp(args.burn_date, args.end, timezone),
        node_ids=tuple(args.nodes),
        aggregation_seconds=args.aggregation_seconds,
        max_connected_gap_seconds=args.max_connected_gap_seconds,
        wind_rolling_window=args.wind_rolling_window,
        pm_max_ug_m3=args.pm_max_ug_m3,
        pm_scale=args.pm_scale,
        event_labels=args.event_labels,
    )
    if settings.end <= settings.start:
        raise ValueError("--end must be later than --start")

    configure_matplotlib()
    telemetry = load_node_telemetry(args.telemetry, settings.node_ids)
    telemetry, pm25_qc = filter_pm25_readings(telemetry, settings.pm_max_ug_m3)
    base_environment = load_base_environment(args.base_environment)
    anemometer_readings = load_anemometer(args.anemometer)
    events = load_events(args.timeline, args.burn_date, timezone)
    nodes = aggregate_nodes(telemetry, settings)
    base = aggregate_base(base_environment, settings)
    anemometer = aggregate_anemometer(anemometer_readings, settings)

    written: list[Path] = []
    written.extend(plot_temperature_humidity(nodes, base, events, settings, args.output_dir, args.also_png))
    written.extend(plot_pm25(nodes, events, settings, args.output_dir, args.also_png))
    written.extend(
        plot_wind_intensity(
            nodes, anemometer, events, settings, args.output_dir, args.also_png
        )
    )
    written.extend(plot_environmental_deviation(nodes, base, events, settings, args.output_dir, args.also_png))
    written.extend(
        plot_synchronized_stack(
            nodes, base, anemometer, events, settings, args.output_dir, args.also_png
        )
    )

    print(
        f"Generated {len(written)} file(s) with {settings.aggregation_seconds}-second medians "
        f"and PM2.5 {settings.pm_scale} scale for nodes "
        f"{', '.join(str(node_id) for node_id in settings.node_ids)}:"
    )
    print(
        "PM2.5 QC masked "
        f"{pm25_qc['total']} raw sample(s): "
        f"over_range={pm25_qc['over_range']}, "
        f"delta_clamped={pm25_qc['delta_clamped']}, "
        f"inconsistent={pm25_qc['inconsistent']}, "
        f"negative={pm25_qc['negative']}"
    )
    for path in written:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
