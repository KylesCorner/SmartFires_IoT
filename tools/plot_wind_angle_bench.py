#!/usr/bin/env python3

import argparse
import io
from pathlib import Path

import holoviews as hv
import hvplot.pandas  # noqa: F401
import pandas as pd

MPS_TO_MPH = 2.2369362921


def load_smartfires_csv(path: Path) -> pd.DataFrame:
    """
    Load a SmartFires serial CSV file.

    Handles files that begin with PlatformIO monitor text like:
        --- Terminal on /dev/ttyACM0 | 115200 8-N-1
        --- Available filters ...
        t_ms,servo_pass,...

    Also handles repeated CSV headers and malformed serial lines.
    """
    raw_lines = path.read_text(errors="replace").splitlines()

    header_index = None
    for i, line in enumerate(raw_lines):
        clean = line.strip()
        if clean.startswith("t_ms,"):
            header_index = i
            break

    if header_index is None:
        raise ValueError("Could not find CSV header line starting with 't_ms,'")

    csv_lines = []
    for line in raw_lines[header_index:]:
        clean = line.strip()

        if not clean:
            continue

        if clean.startswith("---"):
            continue

        if clean.startswith("#"):
            continue

        csv_lines.append(clean)

    csv_text = "\n".join(csv_lines)

    df = pd.read_csv(io.StringIO(csv_text))

    # Remove repeated headers that may appear if the Arduino receives "header".
    df = df[df["t_ms"].astype(str) != "t_ms"].copy()

    numeric_cols = [
        "t_ms",
        "servo_pass",
        "fan_enabled",
        "servo_deg",
        "raw_wind",
        "wind_v",
        "raw_tmp",
        "tmp_v",
        "zero_v",
        "wind_mph",
        "wind_mps",
        "fan_pwm_percent",
        "fan_tach_hz",
        "fan_rpm",
        "fan_tach_pulses_total",
    ]

    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    required_cols = [
        "t_ms",
        "servo_deg",
        "wind_mph",
        "fan_rpm",
    ]

    missing = [col for col in required_cols if col not in df.columns]
    if missing:
        raise ValueError(f"Missing required CSV columns: {missing}")

    df = df.dropna(subset=required_cols).copy()

    # One sample index per valid CSV row, useful because you said to ignore epoch/time.
    df["sample_index"] = range(len(df))

    # Seconds from first valid sample, useful for optional inspection.
    df["t_s"] = (df["t_ms"] - df["t_ms"].iloc[0]) / 1000.0

    return df

def build_fan_state_summary(df: pd.DataFrame) -> pd.DataFrame:
    """
    Calculate wind speed mean/std dev for fan-off and fan-on data.

    std is pandas' default sample standard deviation, ddof=1.
    """
    if "fan_enabled" not in df.columns:
        raise ValueError("CSV is missing required column: fan_enabled")

    summary = (
        df.groupby("fan_enabled", as_index=False)
        .agg(
            sample_count=("wind_mph", "count"),
            wind_mph_mean=("wind_mph", "mean"),
            wind_mph_std=("wind_mph", "std"),
            wind_mph_min=("wind_mph", "min"),
            wind_mph_max=("wind_mph", "max"),
            wind_mps_mean=("wind_mps", "mean"),
            wind_mps_std=("wind_mps", "std"),
            fan_rpm_mean=("fan_rpm", "mean"),
            fan_rpm_std=("fan_rpm", "std"),
        )
    )

    summary["fan_state"] = summary["fan_enabled"].map(
        {
            0: "fan_off_control",
            1: "fan_on_powered",
        }
    )

    return summary[
        [
            "fan_state",
            "fan_enabled",
            "sample_count",
            "wind_mph_mean",
            "wind_mph_std",
            "wind_mph_min",
            "wind_mph_max",
            "wind_mps_mean",
            "wind_mps_std",
            "fan_rpm_mean",
            "fan_rpm_std",
        ]
    ]

def add_fan_wind_estimates(
    df: pd.DataFrame,
    fan_diameter_mm: float,
    tip_speed_coeff: float,
    ref_rpm: float | None,
    ref_wind_mph: float | None,
) -> pd.DataFrame:
    """
    Add estimated wind speed from fan tach RPM.

    Important:
    RPM alone does not uniquely determine outlet wind speed. The actual air speed
    depends on blade geometry, pitch, loading, distance from fan, ducting, blockage,
    and where the wind sensor sits.

    This script supports two modes:

    1. Reference calibration mode:
        fan_est_mph = ref_wind_mph * fan_rpm / ref_rpm

       Use this if you have one known fan speed measurement.

    2. Tip-speed coefficient mode:
        blade_tip_speed_mps = pi * diameter_m * rpm / 60
        fan_est_mps = tip_speed_coeff * blade_tip_speed_mps

       This is only a rough estimate. The coefficient is empirical.
    """
    out = df.copy()

    diameter_m = fan_diameter_mm / 1000.0

    out["fan_tip_speed_mps"] = 3.141592653589793 * diameter_m * out["fan_rpm"] / 60.0
    out["fan_tip_speed_mph"] = out["fan_tip_speed_mps"] * MPS_TO_MPH

    if ref_rpm is not None and ref_wind_mph is not None:
        out["fan_wind_est_mph"] = ref_wind_mph * out["fan_rpm"] / ref_rpm
        out["fan_wind_est_mps"] = out["fan_wind_est_mph"] / MPS_TO_MPH
        out["fan_wind_est_method"] = "reference_calibrated"
    else:
        out["fan_wind_est_mps"] = tip_speed_coeff * out["fan_tip_speed_mps"]
        out["fan_wind_est_mph"] = out["fan_wind_est_mps"] * MPS_TO_MPH
        out["fan_wind_est_method"] = "tip_speed_coefficient"

    return out

def make_plots(df: pd.DataFrame):
    """
    Create hvplot plots.
    """
    servo_wind_long = df[["sample_index", "servo_deg", "wind_mph"]].melt(
        id_vars="sample_index",
        value_vars=["servo_deg", "wind_mph"],
        var_name="signal",
        value_name="value",
    )

    servo_wind_plot = servo_wind_long.hvplot.line(
        x="sample_index",
        y="value",
        by="signal",
        title="Servo angle and wind sensor speed",
        xlabel="Sample index",
        ylabel="Servo degrees / wind mph",
        line_width=2,
        height=400,
        width=1000,
        grid=True,
    )

    wind_compare_long = df[
        ["sample_index", "wind_mph", "fan_wind_est_mph"]
    ].melt(
        id_vars="sample_index",
        value_vars=["wind_mph", "fan_wind_est_mph"],
        var_name="signal",
        value_name="speed_mph",
    )

    wind_compare_plot = wind_compare_long.hvplot.line(
        x="sample_index",
        y="speed_mph",
        by="signal",
        title="Wind sensor speed vs fan-derived estimated wind speed",
        xlabel="Sample index",
        ylabel="Wind speed, mph",
        line_width=2,
        height=400,
        width=1000,
        grid=True,
    )

    rpm_plot = df.hvplot.line(
        x="sample_index",
        y="fan_rpm",
        title="Fan tach RPM",
        xlabel="Sample index",
        ylabel="RPM",
        line_width=2,
        height=300,
        width=1000,
        grid=True,
    )

    # ---------------------------------------------------------
    # Wind speed vs servo angle
    # ---------------------------------------------------------
    # Use only fan-on data for the main angular response.
    # The control pass should stay separate because fan_off wind is expected near zero.
    powered_df = df[df["fan_enabled"] == 1].copy()

    angle_summary = (
        powered_df.groupby("servo_deg", as_index=False)
        .agg(
            wind_mph_mean=("wind_mph", "mean"),
            wind_mph_std=("wind_mph", "std"),
            fan_rpm_mean=("fan_rpm", "mean"),
            sample_count=("wind_mph", "count"),
        )
        .sort_values("servo_deg")
    )

    angle_raw_scatter = powered_df.hvplot.scatter(
        x="servo_deg",
        y="wind_mph",
        title="Wind speed vs servo angle",
        xlabel="Servo angle, degrees",
        ylabel="Wind speed, mph",
        height=400,
        width=1000,
        grid=True,
        alpha=0.25,
        size=20,
        label="Raw samples",
    )

    angle_mean_line = angle_summary.hvplot.line(
        x="servo_deg",
        y="wind_mph_mean",
        xlabel="Servo angle, degrees",
        ylabel="Wind speed, mph",
        line_width=3,
        label="Mean by angle",
    )

    angle_response_plot = angle_raw_scatter * angle_mean_line

    return (
        servo_wind_plot
        + wind_compare_plot
        + rpm_plot
        + angle_response_plot
    ).cols(1)

def print_summary(df: pd.DataFrame) -> None:
    print()
    print("Loaded rows:", len(df))
    print("Servo range deg:", f"{df['servo_deg'].min():.1f} to {df['servo_deg'].max():.1f}")
    print("Wind sensor mph:", f"{df['wind_mph'].min():.3f} to {df['wind_mph'].max():.3f}")
    print("Fan RPM:", f"{df['fan_rpm'].min():.1f} to {df['fan_rpm'].max():.1f}")
    print("Fan estimated wind mph:", f"{df['fan_wind_est_mph'].min():.3f} to {df['fan_wind_est_mph'].max():.3f}")

    fan_state_summary = build_fan_state_summary(df)

    print()
    print("Wind speed summary by fan state:")
    print(
        fan_state_summary.to_string(
            index=False,
            formatters={
                "wind_mph_mean": "{:.3f}".format,
                "wind_mph_std": "{:.3f}".format,
                "wind_mph_min": "{:.3f}".format,
                "wind_mph_max": "{:.3f}".format,
                "wind_mps_mean": "{:.3f}".format,
                "wind_mps_std": "{:.3f}".format,
                "fan_rpm_mean": "{:.1f}".format,
                "fan_rpm_std": "{:.1f}".format,
            },
        )
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot SmartFires wind angle test bench CSV data using hvplot."
    )

    parser.add_argument(
        "csv_file",
        type=Path,
        help="Input CSV file captured from the Arduino serial output.",
    )

    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Output HTML file. Default: <csv_file_stem>_wind_plots.html",
    )

    parser.add_argument(
        "--fan-diameter-mm",
        type=float,
        default=92.0,
        help="Fan diameter in mm. Default: 92.0",
    )

    parser.add_argument(
        "--tip-speed-coeff",
        type=float,
        default=0.30,
        help=(
            "Rough axial wind coefficient for fan estimate. "
            "fan_wind_est = coeff * blade_tip_speed. Default: 0.30"
        ),
    )

    parser.add_argument(
        "--ref-rpm",
        type=float,
        default=None,
        help="Reference fan RPM for calibration mode.",
    )

    parser.add_argument(
        "--ref-wind-mph",
        type=float,
        default=None,
        help="Known wind speed in mph at --ref-rpm for calibration mode.",
    )

    parser.add_argument(
        "--cleaned-csv-out",
        type=Path,
        default=None,
        help="Optional path to save the cleaned/processed CSV.",
    )

    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if args.ref_rpm is None and args.ref_wind_mph is not None:
        raise ValueError("--ref-wind-mph requires --ref-rpm")

    if args.ref_rpm is not None and args.ref_wind_mph is None:
        raise ValueError("--ref-rpm requires --ref-wind-mph")

    if args.ref_rpm is not None and args.ref_rpm <= 0:
        raise ValueError("--ref-rpm must be > 0")

    df = load_smartfires_csv(args.csv_file)

    df = add_fan_wind_estimates(
        df=df,
        fan_diameter_mm=args.fan_diameter_mm,
        tip_speed_coeff=args.tip_speed_coeff,
        ref_rpm=args.ref_rpm,
        ref_wind_mph=args.ref_wind_mph,
    )

    print_summary(df)

    if args.cleaned_csv_out is not None:
        df.to_csv(args.cleaned_csv_out, index=False)
        print()
        print(f"Wrote cleaned CSV: {args.cleaned_csv_out}")

    plot = make_plots(df)

    out_path = args.out
    if out_path is None:
        out_path = args.csv_file.with_name(f"{args.csv_file.stem}_wind_plots.html")

    hv.save(plot, out_path)

    print()
    print(f"Wrote plot HTML: {out_path}")


if __name__ == "__main__":
    main()
