#!/usr/bin/env python3

"""
Parse SmartFires debug logs and generate an interactive HTML plot of:

    - SHT31 temperature versus debug sequence number
    - SHT31 humidity versus debug sequence number

Example:
    python plot_sht31.py smartfires_debug.log

Specify output:
    python plot_sht31.py smartfires_debug.log --output sht31_plot.html

Open the generated plot automatically:
    python plot_sht31.py smartfires_debug.log --show
"""

from __future__ import annotations

import argparse
import re
import sys
import webbrowser
from pathlib import Path

import holoviews as hv
import hvplot.pandas  # noqa: F401
import pandas as pd


# The metadata and SHT31 payload are parsed separately so their order within
# the log line does not matter.
SEQ_PATTERN = re.compile(r"\bseq=(?P<seq>\d+)\b")

SHT31_PATTERN = re.compile(
    r"\bsht31,"
    r"temp_c=(?P<temp_c>-?\d+(?:\.\d+)?),"
    r"humidity_pct=(?P<humidity_pct>-?\d+(?:\.\d+)?),"
    r"valid=(?P<valid>\d+),"
    r"t_ms=(?P<t_ms>\d+)\b"
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot SHT31 temperature and humidity from SmartFires logs."
    )

    parser.add_argument(
        "log_file",
        type=Path,
        help="Path to the SmartFires debug log.",
    )

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("sht31_plot.html"),
        help="Output HTML file. Default: sht31_plot.html",
    )

    parser.add_argument(
        "--show",
        action="store_true",
        help="Open the generated HTML plot in the default browser.",
    )

    parser.add_argument(
        "--include-invalid",
        action="store_true",
        help="Include SHT31 samples where valid is not 1.",
    )

    return parser.parse_args()


def parse_sht31_log(log_file: Path) -> pd.DataFrame:
    rows: list[dict[str, int | float]] = []

    with log_file.open("r", encoding="utf-8", errors="replace") as handle:
        for line_number, line in enumerate(handle, start=1):
            # Ignore unrelated SHT31 status messages and all other sources.
            if "sht31,temp_c=" not in line:
                continue

            seq_match = SEQ_PATTERN.search(line)
            sht_match = SHT31_PATTERN.search(line)

            if seq_match is None or sht_match is None:
                print(
                    f"Warning: could not parse SHT31 line {line_number}: "
                    f"{line.rstrip()}",
                    file=sys.stderr,
                )
                continue

            rows.append(
                {
                    "seq": int(seq_match.group("seq")),
                    "temp_c": float(sht_match.group("temp_c")),
                    "humidity_pct": float(
                        sht_match.group("humidity_pct")
                    ),
                    "valid": int(sht_match.group("valid")),
                    "t_ms": int(sht_match.group("t_ms")),
                }
            )

    if not rows:
        raise ValueError(
            "No SHT31 measurement lines were found. Expected lines containing "
            "'sht31,temp_c='."
        )

    dataframe = pd.DataFrame(rows)

    dataframe = (
        dataframe.sort_values(["seq", "t_ms"])
        .drop_duplicates(subset=["seq"], keep="last")
        .reset_index(drop=True)
    )

    return dataframe


def create_plot(dataframe: pd.DataFrame) -> hv.Layout:
    temperature_plot = dataframe.hvplot.line(
        x="seq",
        y="temp_c",
        marker="circle",
        hover_cols=["t_ms", "valid", "humidity_pct"],
        title="SHT31 Temperature",
        xlabel="Debug Sequence Number",
        ylabel="Temperature (°C)",
        width=1000,
        height=350,
        grid=True,
    )

    humidity_plot = dataframe.hvplot.line(
        x="seq",
        y="humidity_pct",
        marker="circle",
        hover_cols=["t_ms", "valid", "temp_c"],
        title="SHT31 Relative Humidity",
        xlabel="Debug Sequence Number",
        ylabel="Relative Humidity (%)",
        width=1000,
        height=350,
        grid=True,
    )

    return (temperature_plot + humidity_plot).cols(1)


def main() -> int:
    args = parse_arguments()

    if not args.log_file.is_file():
        print(
            f"Error: log file does not exist: {args.log_file}",
            file=sys.stderr,
        )
        return 1

    try:
        dataframe = parse_sht31_log(args.log_file)
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1

    if not args.include_invalid:
        dataframe = dataframe[dataframe["valid"] == 1].copy()

    if dataframe.empty:
        print(
            "Error: no valid SHT31 samples remain after filtering.",
            file=sys.stderr,
        )
        return 1

    hv.extension("bokeh")

    plot = create_plot(dataframe)

    output_path = args.output.expanduser().resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    hv.save(
        plot,
        output_path,
        backend="bokeh",
        resources="cdn",
        title="SmartFires SHT31 Measurements",
    )

    print(f"Parsed samples: {len(dataframe)}")
    print(
        f"Sequence range: {dataframe['seq'].min()} "
        f"to {dataframe['seq'].max()}"
    )
    print(
        f"Temperature range: {dataframe['temp_c'].min():.2f} "
        f"to {dataframe['temp_c'].max():.2f} °C"
    )
    print(
        f"Humidity range: {dataframe['humidity_pct'].min():.2f} "
        f"to {dataframe['humidity_pct'].max():.2f} %"
    )
    print(f"Plot written to: {output_path}")

    if args.show:
        webbrowser.open(output_path.as_uri())

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
