import argparse
import time

from .csv_writer import SensorCsvWriter
from .manager import SensorManager


def main():
    parser = argparse.ArgumentParser(
        description="SmartFires edge sensor sampler"
    )

    parser.add_argument(
        "--i2c-bus",
        type=int,
        default=7,
        help="Linux I2C bus number",
    )

    parser.add_argument(
        "--anemometer-port",
        default="/dev/ttyUSB0",
        help="ES-W302 RS485 serial device",
    )

    parser.add_argument(
        "--imu-rate",
        type=float,
        default=50.0,
        help="IMU sample rate in Hz",
    )

    parser.add_argument(
        "--gps-rate",
        type=float,
        default=1.0,
        help="GPS sample rate in Hz",
    )

    parser.add_argument(
        "--bme-rate",
        type=float,
        default=1.0,
        help="BME688 sample rate in Hz",
    )

    parser.add_argument(
        "--anemometer-rate",
        type=float,
        default=1.0,
        help="Anemometer sample rate in Hz",
    )

    parser.add_argument(
        "--output-dir",
        default="sensor-data",
        help="Directory used for CSV sensor logs",
    )

    args = parser.parse_args()

    print("Initializing SmartFires sensors...")

    sensors = SensorManager(
        i2c_bus=args.i2c_bus,
        anemometer_port=args.anemometer_port,
    )

    print("Sensors initialized")

    imu_period = 1.0 / args.imu_rate
    gps_period = 1.0 / args.gps_rate
    bme_period = 1.0 / args.bme_rate
    anemometer_period = (
        1.0 / args.anemometer_rate
    )

    now = time.monotonic()

    next_imu = now
    next_gps = now
    next_bme = now
    next_anemometer = now

    with SensorCsvWriter(
        args.output_dir
    ) as csv_writer:

        print(
            f"CSV logs: "
            f"{csv_writer.session_dir}"
        )

        print("Press Ctrl+C to stop")
        print()

        try:
            while True:

                now = time.monotonic()

                # --------------------------------------------------
                # GPS service
                #
                # Keep consuming incoming GPS/NMEA data regardless
                # of the configured GPS logging rate.
                # --------------------------------------------------

                try:
                    sensors.gps.update()

                except Exception as exc:
                    print(
                        f"[GPS UPDATE ERROR] {exc}"
                    )

                # --------------------------------------------------
                # IMU
                # --------------------------------------------------

                if now >= next_imu:

                    try:
                        sample = sensors.imu.sample()

                        csv_writer.write(
                            "imu",
                            sample,
                        )

                        print(
                            "IMU "
                            f"accel=("
                            f"{sample['accel_x_mps2']:.3f}, "
                            f"{sample['accel_y_mps2']:.3f}, "
                            f"{sample['accel_z_mps2']:.3f}) "
                            f"m/s^2 "
                            f"gyro=("
                            f"{sample['gyro_x_dps']:.3f}, "
                            f"{sample['gyro_y_dps']:.3f}, "
                            f"{sample['gyro_z_dps']:.3f}) "
                            f"deg/s"
                        )

                    except Exception as exc:
                        print(
                            f"[IMU ERROR] {exc}"
                        )

                    next_imu += imu_period

                # --------------------------------------------------
                # GPS
                # --------------------------------------------------

                if now >= next_gps:

                    try:
                        sample = sensors.gps.sample()

                        csv_writer.write(
                            "gps",
                            sample,
                        )

                        print(
                            "GPS "
                            f"timestamp="
                            f"{sample['timestamp_utc']} "
                            f"fix={sample['has_fix']} "
                            f"lat={sample['latitude']} "
                            f"lon={sample['longitude']} "
                            f"alt={sample['altitude_m']} "
                            f"sats={sample['satellites']}"
                        )

                    except Exception as exc:
                        print(
                            f"[GPS ERROR] {exc}"
                        )

                    next_gps += gps_period

                # --------------------------------------------------
                # BME688
                # --------------------------------------------------

                if now >= next_bme:

                    try:
                        sample = sensors.bme688.sample()

                        csv_writer.write(
                            "bme688",
                            sample,
                        )

                        print(
                            "BME688 "
                            f"temp="
                            f"{sample['temperature_c']:.2f} C "
                            f"humidity="
                            f"{sample['humidity_percent']:.2f}% "
                            f"pressure="
                            f"{sample['pressure_hpa']:.2f} hPa "
                            f"gas="
                            f"{sample['gas_resistance_ohm']} ohm"
                        )

                    except Exception as exc:
                        print(
                            f"[BME688 ERROR] {exc}"
                        )

                    next_bme += bme_period

                # --------------------------------------------------
                # ES-W302
                # --------------------------------------------------

                if now >= next_anemometer:

                    try:
                        sample = (
                            sensors.anemometer.sample()
                        )

                        csv_writer.write(
                            "anemometer",
                            sample,
                        )

                        print(
                            "ANEMOMETER "
                            f"speed="
                            f"{sample['speed_mps']:.3f} m/s "
                            f"direction="
                            f"{sample['direction_deg']} deg"
                        )

                    except Exception as exc:
                        print(
                            f"[ANEMOMETER ERROR] {exc}"
                        )

                    next_anemometer += (
                        anemometer_period
                    )

                time.sleep(0.001)

        except KeyboardInterrupt:
            print()
            print(
                "Stopping SmartFires sensor sampler..."
            )


if __name__ == "__main__":
    main()
