#!/usr/bin/env python3

import argparse
import calendar
import math
import os
import sys
import threading
import time
from pathlib import Path

# --------------------------------------------------------------------
# Jetson / Blinka workaround
#
# Must happen BEFORE importing Adafruit Blinka libraries.
# --------------------------------------------------------------------

os.environ.setdefault("JETSON_MODEL_NAME", "JETSON_ORIN_NANO")


# --------------------------------------------------------------------
# SmartFires edge-receiver imports
# --------------------------------------------------------------------

EDGE_RECEIVER_SRC = (
    Path(__file__).resolve().parent
    / "edge-receiver"
    / "src"
)

if str(EDGE_RECEIVER_SRC) not in sys.path:
    sys.path.insert(0, str(EDGE_RECEIVER_SRC))

from smartfires_edge.anemometer import (
    DEFAULT_ADDRESS,
    DEFAULT_BAUD,
    make_instrument,
    read_once,
)


# --------------------------------------------------------------------
# I2C sensor imports
# --------------------------------------------------------------------

from adafruit_extended_bus import ExtendedI2C
import adafruit_icm20x
import adafruit_bme680
import adafruit_gps


# --------------------------------------------------------------------
# Configuration
# --------------------------------------------------------------------

I2C_BUS = 7

IMU_ADDRESS = 0x69
BME_ADDRESS = 0x77

IMU_RATE_HZ = 50.0
BME_RATE_HZ = 1.0
GPS_RATE_HZ = 1.0

ANEMOMETER_PORT = "/dev/ttyUSB0"
ANEMOMETER_RATE_HZ = 1.0


# --------------------------------------------------------------------
# Anemometer worker
# --------------------------------------------------------------------

class AnemometerWorker:
    def __init__(
        self,
        port,
        baud,
        address,
        rate_hz=1.0,
        debug=False,
    ):
        self.port = port
        self.baud = baud
        self.address = address
        self.period = 1.0 / rate_hz

        self.instrument = make_instrument(
            port,
            baud,
            address,
        )

        self.instrument.debug = debug

        self._lock = threading.Lock()
        self._stop_event = threading.Event()

        self._speed = None
        self._direction = None
        self._host_epoch_ms = None
        self._error = None

        self._thread = threading.Thread(
            target=self._run,
            name="anemometer",
            daemon=True,
        )

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop_event.set()
        self._thread.join(timeout=2.0)

    def latest(self):
        with self._lock:
            return {
                "host_epoch_ms": self._host_epoch_ms,
                "speed_mps": self._speed,
                "direction_deg": self._direction,
                "error": self._error,
            }

    def _run(self):
        while not self._stop_event.is_set():
            start = time.monotonic()

            try:
                speed, direction = read_once(
                    self.instrument
                )

                host_epoch_ms = time.time_ns() // 1_000_000

                with self._lock:
                    self._speed = speed
                    self._direction = direction
                    self._host_epoch_ms = host_epoch_ms
                    self._error = None

            except Exception as exc:
                with self._lock:
                    self._error = str(exc)

                try:
                    self.instrument.serial.reset_input_buffer()
                except Exception:
                    pass

            elapsed = time.monotonic() - start
            remaining = self.period - elapsed

            if remaining > 0:
                self._stop_event.wait(remaining)


# --------------------------------------------------------------------
# Main
# --------------------------------------------------------------------

def main():

    parser = argparse.ArgumentParser(
        description="SmartFires edge sensor sampler"
    )

    parser.add_argument(
        "--anemometer-port",
        default=ANEMOMETER_PORT,
        help="ES-W302 USB-RS485 serial port",
    )

    parser.add_argument(
        "--anemometer-baud",
        default=DEFAULT_BAUD,
        type=int,
    )

    parser.add_argument(
        "--anemometer-address",
        default=DEFAULT_ADDRESS,
        type=int,
    )

    parser.add_argument(
        "--anemometer-rate",
        default=ANEMOMETER_RATE_HZ,
        type=float,
        help="ES-W302 polling rate in Hz",
    )

    parser.add_argument(
        "--anemometer-debug",
        action="store_true",
    )

    args = parser.parse_args()


    # ----------------------------------------------------------------
    # I2C initialization
    # ----------------------------------------------------------------

    print(f"Opening /dev/i2c-{I2C_BUS}")

    i2c = ExtendedI2C(I2C_BUS)


    # ----------------------------------------------------------------
    # IMU
    # ----------------------------------------------------------------

    print(
        f"Initializing ICM-20948 at "
        f"0x{IMU_ADDRESS:02X}"
    )

    imu = adafruit_icm20x.ICM20948(
        i2c,
        address=IMU_ADDRESS,
    )

    print("ICM-20948 OK")


    # ----------------------------------------------------------------
    # BME688
    # ----------------------------------------------------------------

    print(
        f"Initializing BME688 at "
        f"0x{BME_ADDRESS:02X}"
    )

    bme = adafruit_bme680.Adafruit_BME680_I2C(
        i2c,
        address=BME_ADDRESS,
    )

    print("BME688 OK")


    # ----------------------------------------------------------------
    # GPS
    # ----------------------------------------------------------------

    print("Initializing PA1010D at 0x10")

    gps = adafruit_gps.GPS_GtopI2C(
        i2c,
        debug=False,
    )

    # RMC + GGA
    gps.send_command(
        b"PMTK314,0,1,0,1,0,0,0,0,0,"
        b"0,0,0,0,0,0,0,0,0"
    )

    # 1 Hz navigation update
    gps.send_command(
        b"PMTK220,1000"
    )

    print("PA1010D OK")


    # ----------------------------------------------------------------
    # Anemometer
    # ----------------------------------------------------------------

    print(
        "Initializing ES-W302 "
        f"{args.anemometer_port} "
        f"{args.anemometer_baud} baud "
        f"addr={args.anemometer_address}"
    )

    anemometer = AnemometerWorker(
        port=args.anemometer_port,
        baud=args.anemometer_baud,
        address=args.anemometer_address,
        rate_hz=args.anemometer_rate,
        debug=args.anemometer_debug,
    )

    anemometer.start()

    print("ES-W302 worker started")


    # ----------------------------------------------------------------
    # Scheduling
    # ----------------------------------------------------------------

    imu_period = 1.0 / IMU_RATE_HZ
    bme_period = 1.0 / BME_RATE_HZ
    gps_period = 1.0 / GPS_RATE_HZ
    anemometer_print_period = 1.0 / args.anemometer_rate

    now = time.monotonic()

    next_imu = now
    next_bme = now
    next_gps = now
    next_anemometer = now


    print()
    print("Sampling sensors...")
    print("Ctrl+C to stop")
    print()


    try:

        while True:

            now = time.monotonic()


            # --------------------------------------------------------
            # GPS parser
            #
            # Service continuously even though fixes are 1 Hz.
            # --------------------------------------------------------

            gps.update()


            # --------------------------------------------------------
            # IMU
            # --------------------------------------------------------

            if now >= next_imu:

                host_epoch_ms = (
                    time.time_ns() // 1_000_000
                )

                ax, ay, az = imu.acceleration
                gx, gy, gz = imu.gyro
                mx, my, mz = imu.magnetic

                gx = math.degrees(gx)
                gy = math.degrees(gy)
                gz = math.degrees(gz)

                print(
                    "IMU "
                    f"host_ms={host_epoch_ms} "
                    f"accel=("
                    f"{ax:.3f},"
                    f"{ay:.3f},"
                    f"{az:.3f}) "
                    f"gyro=("
                    f"{gx:.3f},"
                    f"{gy:.3f},"
                    f"{gz:.3f}) "
                    f"mag=("
                    f"{mx:.3f},"
                    f"{my:.3f},"
                    f"{mz:.3f})"
                )

                next_imu += imu_period


            # --------------------------------------------------------
            # BME688
            # --------------------------------------------------------

            if now >= next_bme:

                host_epoch_ms = (
                    time.time_ns() // 1_000_000
                )

                print(
                    "BME688 "
                    f"host_ms={host_epoch_ms} "
                    f"T={bme.temperature:.2f}C "
                    f"RH={bme.relative_humidity:.2f}% "
                    f"P={bme.pressure:.2f}hPa "
                    f"gas={bme.gas}ohm"
                )

                next_bme += bme_period


            # --------------------------------------------------------
            # GPS
            # --------------------------------------------------------

            if now >= next_gps:

                host_epoch_ms = (
                    time.time_ns() // 1_000_000
                )

                gps_epoch_ms = None

                if gps.timestamp_utc:
                    print(gps.timestamp_utc)


                if gps.has_fix:

                    print(
                        "GPS "
                        f"host_ms={host_epoch_ms} "
                        f"gps_ms={gps_epoch_ms} "
                        f"lat={gps.latitude} "
                        f"lon={gps.longitude} "
                        f"alt={gps.altitude_m} "
                        f"sats={gps.satellites}"
                    )

                else:

                    print(
                        "GPS "
                        f"host_ms={host_epoch_ms} "
                        f"gps_ms={gps_epoch_ms} "
                        "waiting_for_fix"
                    )

                next_gps += gps_period


            # --------------------------------------------------------
            # ES-W302
            # --------------------------------------------------------

            if now >= next_anemometer:

                sample = anemometer.latest()

                if sample["error"] is not None:

                    print(
                        "ANEMOMETER "
                        f"ERROR={sample['error']}"
                    )

                elif sample["speed_mps"] is not None:

                    print(
                        "ANEMOMETER "
                        f"host_ms="
                        f"{sample['host_epoch_ms']} "
                        f"speed="
                        f"{sample['speed_mps']:.3f}m/s "
                        f"direction="
                        f"{sample['direction_deg']}deg"
                    )

                else:

                    print(
                        "ANEMOMETER "
                        "waiting_for_first_sample"
                    )

                next_anemometer += (
                    anemometer_print_period
                )


            # --------------------------------------------------------
            # Prevent busy spinning
            # --------------------------------------------------------

            time.sleep(0.001)


    except KeyboardInterrupt:
        print()
        print("Stopping...")

    finally:
        anemometer.stop()


if __name__ == "__main__":
    main()
