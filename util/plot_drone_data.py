#!/usr/bin/env python3

import argparse
import subprocess
from pathlib import Path

import holoviews as hv
import hvplot.pandas  # noqa: F401
import pandas as pd

hv.extension("bokeh")


DEFAULT_RSYNC_SOURCE = "jetson:/mnt/nvme_drive/data/"
DEFAULT_RSYNC_DEST = "/run/media/kyle/External/SmartFires/"


COLORS = {
    "wind_mps": {
        "1": "#2ca02c",
        "2": "#98df8a",
    },
    "temp_c": {
        "1": "#d62728",
        "2": "#ff9896",
    },
    "humidity_pct": {
        "1": "#1f77b4",
        "2": "#aec7e8",
    },
    "pm1_0_ug_m3": {
        "1": "#9467bd",
        "2": "#c5b0d5",
    },
    "pm2_5_ug_m3": {
        "1": "#ff7f0e",
        "2": "#ffbb78",
    },
    "pm4_0_ug_m3": {
        "1": "#8c564b",
        "2": "#c49c94",
    },
    "pm10_ug_m3": {
        "1": "#17becf",
        "2": "#9edae5",
    },
}


def run_rsync(source: str, dest: str) -> None:
    print(f"Syncing data from {source} to {dest}")

    subprocess.run(
        ["rsync", "-avh", "--progress", source, dest],
        check=True,
    )

    print("Rsync complete.")


def load_data(csv_path: Path) -> tuple[pd.DataFrame, str]:
    df = pd.read_csv(csv_path)

    df["timestamp"] = pd.to_datetime(df["timestamp"], errors="coerce")

    if df["timestamp"].notna().any():
        x_col = "timestamp"
    else:
        df["session_time_s"] = df["session_time_ms"] / 1000.0
        x_col = "session_time_s"

    df["node_id"] = df["node_id"].astype(str)
    df = df.sort_values(["node_id", x_col])

    return df, x_col


def add_series_and_colors(
    long_df: pd.DataFrame,
    measurement_col: str,
) -> pd.DataFrame:
    long_df = long_df.copy()

    long_df["series"] = long_df[measurement_col] + " - Node " + long_df["node_id"]

    long_df["color"] = long_df.apply(
        lambda row: COLORS.get(row[measurement_col], {}).get(row["node_id"], "gray"),
        axis=1,
    )

    return long_df


def make_plots(df: pd.DataFrame, x_col: str):
    wind_df = df.copy()
    wind_df["color"] = wind_df["node_id"].map(COLORS["wind_mps"]).fillna("gray")
    wind_df["series"] = "wind_mps - Node " + wind_df["node_id"]

    wind_plot = wind_df.hvplot.scatter(
        x=x_col,
        y="wind_mps",
        by="series",
        color="color",
        title="Wind Magnitude by Node",
        xlabel="Time",
        ylabel="Wind Speed (m/s)",
        size=2,
        alpha=0.7,
        width=1000,
        height=400,
        grid=True,
    )

    env_long = df.melt(
        id_vars=[x_col, "node_id"],
        value_vars=["temp_c", "humidity_pct"],
        var_name="measurement",
        value_name="value",
    )
    env_long = add_series_and_colors(env_long, "measurement")

    temp_humidity_plot = env_long.hvplot.scatter(
        x=x_col,
        y="value",
        by="series",
        color="color",
        title="Temperature and Humidity by Node",
        xlabel="Time",
        ylabel="Value",
        size=2,
        alpha=0.7,
        width=1000,
        height=450,
        grid=True,
    )

    pm_long = df.melt(
        id_vars=[x_col, "node_id"],
        value_vars=[
            "pm1_0_ug_m3",
            "pm2_5_ug_m3",
            "pm4_0_ug_m3",
            "pm10_ug_m3",
        ],
        var_name="pm_type",
        value_name="concentration",
    )
    pm_long = add_series_and_colors(pm_long, "pm_type")

    pm_plot = pm_long.hvplot.scatter(
        x=x_col,
        y="concentration",
        by="series",
        color="color",
        title="Particulate Matter by Node",
        xlabel="Time",
        ylabel="PM Concentration (µg/m³)",
        size=2,
        alpha=0.7,
        width=1000,
        height=500,
        grid=True,
    )

    return wind_plot, temp_humidity_plot, pm_plot


def main():
    parser = argparse.ArgumentParser(
        description="Plot SmartFires telemetry CSV data using pandas and hvplot."
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

    df, x_col = load_data(args.csv)
    wind_plot, temp_humidity_plot, pm_plot = make_plots(df, x_col)

    dashboard = (wind_plot + temp_humidity_plot + pm_plot).cols(1)

    hv.save(dashboard, args.output)

    print(f"Saved plots to: {args.output}")


if __name__ == "__main__":
    main()
