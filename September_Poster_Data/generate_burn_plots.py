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
DEFAULT_TIMELINE = DEFAULT_BURN_DIR / "BURN_TIMELINE.txt"
DEFAULT_OUTPUT_DIR = SCRIPT_DIR / "generated_plots"

NODE_IDS = (2, 3, 4, 5)
NODE_COLORS = {
    2: "#2D6A8A",  # poster blue
    3: "#E67E22",  # poster orange
    4: "#66885D",  # poster green
    5: "#8D5A97",  # poster purple
}
BASE_COLOR = "#455564"
TEXT_COLOR = "#17324D"
MUTED_TEXT = "#647683"
GRID_COLOR = "#DCE3E8"
AXIS_COLOR = "#758793"
PANEL_COLOR = "#FBFCFD"

EVENT_STYLES = {
    "run": {"color": "#17324D", "linestyle": "-", "linewidth": 1.25, "alpha": 0.72},
    "ignition": {"color": "#D96D12", "linestyle": "-", "linewidth": 1.8, "alpha": 0.95},
    "fuel": {"color": "#8A2637", "linestyle": "--", "linewidth": 1.1, "alpha": 0.75},
    "last_wood": {"color": "#8A2637", "linestyle": "-", "linewidth": 2.0, "alpha": 0.95},
    "fire": {"color": "#D96D12", "linestyle": ":", "linewidth": 1.2, "alpha": 0.78},
    "equipment": {"color": "#7D8C97", "linestyle": ":", "linewidth": 1.0, "alpha": 0.72},
    "environment": {"color": "#66885D", "linestyle": ":", "linewidth": 1.1, "alpha": 0.78},
}


@dataclass(frozen=True)
class PlotSettings:
    timezone: ZoneInfo
    start: pd.Timestamp
    end: pd.Timestamp
    aggregation_seconds: int
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


def load_node_telemetry(path: Path, node_ids: Sequence[int] = NODE_IDS) -> pd.DataFrame:
    """Load, filter, and de-duplicate node sensor samples."""
    frame = pd.read_csv(path, low_memory=False)
    frame = frame.loc[frame["packet_type"].eq("telemetry")].copy()
    frame["node_id"] = pd.to_numeric(frame["node_id"], errors="coerce")
    frame = frame.loc[frame["node_id"].isin(node_ids)].copy()
    frame["node_id"] = frame["node_id"].astype(int)
    frame["timestamp"] = pd.to_datetime(frame["timestamp"], utc=True, errors="coerce")
    frame = frame.dropna(subset=["timestamp"])

    numeric_columns = ["session_time_ms", "rssi", "temp_c", "humidity_pct", "pm2_5_ug_m3", "wind_mps"]
    for column in numeric_columns:
        frame[column] = pd.to_numeric(frame[column], errors="coerce")

    frame = (
        frame.sort_values("rssi", ascending=False, na_position="last")
        .drop_duplicates(subset=["node_id", "session_time_ms"], keep="first")
        .sort_values(["node_id", "timestamp"])
        .reset_index(drop=True)
    )
    return frame


def load_base_environment(path: Path) -> pd.DataFrame:
    """Load the Jetson BME688 stream used as the environmental reference."""
    frame = pd.read_csv(path)
    frame["timestamp"] = pd.to_datetime(frame["host_epoch_ms"], unit="ms", utc=True)
    for column in ("temperature_c", "humidity_percent"):
        frame[column] = pd.to_numeric(frame[column], errors="coerce")
    return frame.sort_values("timestamp").reset_index(drop=True)


def aggregate_nodes(frame: pd.DataFrame, settings: PlotSettings) -> dict[int, pd.DataFrame]:
    """Aggregate each node with medians and leave empty bins as NaN gaps."""
    rule = f"{settings.aggregation_seconds}s"
    columns = ["temp_c", "humidity_pct", "pm2_5_ug_m3", "wind_mps"]
    result: dict[int, pd.DataFrame] = {}
    for node_id in NODE_IDS:
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


def node_legend_handles(include_base: bool = False) -> list[Line2D]:
    handles = [
        Line2D([0], [0], color=NODE_COLORS[node_id], lw=2.2, label=f"Node {node_id}")
        for node_id in NODE_IDS
    ]
    if include_base:
        handles.append(Line2D([0], [0], color=BASE_COLOR, lw=2.5, label="Base station"))
    return handles


def add_figure_legend(fig: Figure, include_base: bool, include_variables: bool = False) -> None:
    node_legend = fig.legend(
        handles=node_legend_handles(include_base),
        loc="upper center",
        bbox_to_anchor=(0.5, 0.935),
        ncol=5 if include_base else 4,
        frameon=False,
        fontsize=8.5,
        handlelength=2.6,
        columnspacing=1.3,
    )
    node_legend.set_gid("legend-nodes")
    if include_variables:
        variable_handles = [
            Line2D([0], [0], color=BASE_COLOR, lw=1.8, linestyle="-", label="Temperature"),
            Line2D([0], [0], color=BASE_COLOR, lw=1.8, linestyle="--", label="Relative humidity"),
        ]
        variable_legend = fig.legend(
            handles=variable_handles,
            loc="upper center",
            bbox_to_anchor=(0.5, 0.895),
            ncol=2,
            frameon=False,
            fontsize=8.2,
            handlelength=2.8,
            columnspacing=1.5,
        )
        variable_legend.set_gid("legend-variables")


def add_method_note(fig: Figure, settings: PlotSettings, extra: str = "") -> None:
    note = (
        f"Traces show {settings.aggregation_seconds}-second medians; "
        "gaps indicate unavailable recorded data."
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
                xytext=(0, 7 + 8 * (event_index % 2)),
                textcoords="offset points",
                ha="left",
                va="bottom",
                rotation=90,
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
        temperature_line, = ax.plot(
            frame.index,
            frame["temp_c"],
            color=NODE_COLORS[node_id],
            linewidth=1.55,
            linestyle="-",
            zorder=3,
        )
        temperature_line.set_gid(f"{prefix}-node-{node_id}-temperature")
        humidity_line, = humidity_ax.plot(
            frame.index,
            frame["humidity_pct"],
            color=NODE_COLORS[node_id],
            linewidth=1.35,
            linestyle="--",
            dashes=(5, 2.4),
            alpha=0.9,
            zorder=2.8,
        )
        humidity_line.set_gid(f"{prefix}-node-{node_id}-humidity")

    base_temperature, = ax.plot(
        base.index,
        base["temperature_c"],
        color=BASE_COLOR,
        linewidth=2.25,
        linestyle="-",
        zorder=3.3,
    )
    base_temperature.set_gid(f"{prefix}-base-temperature")
    base_humidity, = humidity_ax.plot(
        base.index,
        base["humidity_percent"],
        color=BASE_COLOR,
        linewidth=2.0,
        linestyle="--",
        dashes=(5, 2.4),
        alpha=0.92,
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
        line, = ax.plot(
            frame.index,
            values,
            color=NODE_COLORS[node_id],
            linewidth=1.65,
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
    settings: PlotSettings,
    prefix: str,
) -> None:
    style_axis(ax, "Node wind intensity (m/s)")
    for node_id, frame in nodes.items():
        line, = ax.plot(
            frame.index,
            frame["wind_mps"],
            color=NODE_COLORS[node_id],
            linewidth=1.55,
            zorder=3,
        )
        line.set_gid(f"{prefix}-node-{node_id}-wind")
    ax.set_ylim(bottom=0)
    apply_time_axis(ax, settings)


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
    add_figure_legend(fig, include_base=True, include_variables=True)
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
    add_figure_legend(fig, include_base=False)
    add_method_note(fig, settings)
    fig.subplots_adjust(left=0.075, right=0.975, bottom=0.16, top=0.68)
    return save_figure(fig, output_dir / "pm25.svg", also_png)


def plot_wind_intensity(
    nodes: dict[int, pd.DataFrame],
    events: Sequence[BurnEvent],
    settings: PlotSettings,
    output_dir: Path,
    also_png: bool,
) -> list[Path]:
    fig, ax = plt.subplots(figsize=(13.36, 4.8))
    fig.suptitle("Node Wind Intensity", fontsize=15, fontweight="bold", y=0.985)
    draw_wind(ax, nodes, settings, "wind")
    add_events([ax], events, settings, ax, "wind")
    add_figure_legend(fig, include_base=False)
    add_method_note(fig, settings, "Node wind values are shown as reported.")
    fig.subplots_adjust(left=0.075, right=0.975, bottom=0.16, top=0.68)
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
        zero_line = ax.axhline(0, color=BASE_COLOR, linewidth=1.0, alpha=0.8, zorder=2)
        zero_line.set_gid(f"deviation-{variable_name}-zero-reference")
        for node_id, frame in deviations.items():
            line, = ax.plot(
                frame.index,
                frame[column],
                color=NODE_COLORS[node_id],
                linewidth=1.6,
                zorder=3,
            )
            line.set_gid(f"deviation-node-{node_id}-{variable_name}")
    apply_time_axis(axes[0], settings, show_labels=False)
    apply_time_axis(axes[1], settings, show_labels=True)
    add_events(axes, events, settings, axes[0], "deviation")
    add_figure_legend(fig, include_base=False)
    add_method_note(fig, settings, "Differences use the aligned Jetson BME688 base-station record.")
    fig.subplots_adjust(left=0.09, right=0.975, bottom=0.11, top=0.76, hspace=0.16)
    return save_figure(fig, output_dir / "environmental_deviation.svg", also_png)


def plot_synchronized_stack(
    nodes: dict[int, pd.DataFrame],
    base: pd.DataFrame,
    events: Sequence[BurnEvent],
    settings: PlotSettings,
    output_dir: Path,
    also_png: bool,
) -> list[Path]:
    fig, axes = plt.subplots(
        3,
        1,
        figsize=(13.36, 12.4),
        sharex=True,
        gridspec_kw={"height_ratios": [1.28, 1.0, 1.0]},
    )
    fig.suptitle("Synchronized Sensor Response", fontsize=16, fontweight="bold", y=0.992)
    draw_temperature_humidity(axes[0], nodes, base, settings, "stack-temperature-humidity")
    draw_wind(axes[1], nodes, settings, "stack-wind")
    draw_pm25(axes[2], nodes, settings, "stack-pm25")
    apply_time_axis(axes[0], settings, show_labels=False)
    apply_time_axis(axes[1], settings, show_labels=False)
    apply_time_axis(axes[2], settings, show_labels=True)
    add_events(axes, events, settings, axes[0], "stack")
    add_figure_legend(fig, include_base=True, include_variables=True)
    add_method_note(fig, settings, "Node wind values are shown as reported.")
    fig.subplots_adjust(left=0.077, right=0.925, bottom=0.075, top=0.79, hspace=0.14)
    return save_figure(fig, output_dir / "synchronized_sensor_response.svg", also_png)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate editable SVG plots for the September 15 SmartFires burn.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--telemetry", type=Path, default=DEFAULT_TELEMETRY)
    parser.add_argument("--base-environment", type=Path, default=DEFAULT_BASE_ENVIRONMENT)
    parser.add_argument("--timeline", type=Path, default=DEFAULT_TIMELINE)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--burn-date", type=date.fromisoformat, default=date(2026, 9, 15))
    parser.add_argument("--timezone", default="America/Denver")
    parser.add_argument("--start", type=parse_clock, default=time(16, 10), help="24-hour local HH:MM")
    parser.add_argument("--end", type=parse_clock, default=time(20, 30), help="24-hour local HH:MM")
    parser.add_argument(
        "--aggregation-seconds",
        type=int,
        default=15,
        help="median aggregation window; empty windows remain gaps",
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
    for path in (args.telemetry, args.base_environment, args.timeline):
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
        aggregation_seconds=args.aggregation_seconds,
        pm_scale=args.pm_scale,
        event_labels=args.event_labels,
    )
    if settings.end <= settings.start:
        raise ValueError("--end must be later than --start")

    configure_matplotlib()
    telemetry = load_node_telemetry(args.telemetry)
    base_environment = load_base_environment(args.base_environment)
    events = load_events(args.timeline, args.burn_date, timezone)
    nodes = aggregate_nodes(telemetry, settings)
    base = aggregate_base(base_environment, settings)

    written: list[Path] = []
    written.extend(plot_temperature_humidity(nodes, base, events, settings, args.output_dir, args.also_png))
    written.extend(plot_pm25(nodes, events, settings, args.output_dir, args.also_png))
    written.extend(plot_wind_intensity(nodes, events, settings, args.output_dir, args.also_png))
    written.extend(plot_environmental_deviation(nodes, base, events, settings, args.output_dir, args.also_png))
    written.extend(plot_synchronized_stack(nodes, base, events, settings, args.output_dir, args.also_png))

    print(
        f"Generated {len(written)} file(s) with {settings.aggregation_seconds}-second medians "
        f"and PM2.5 {settings.pm_scale} scale:"
    )
    for path in written:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
