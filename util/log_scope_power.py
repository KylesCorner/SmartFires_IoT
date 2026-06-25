import argparse
import csv
import re
import socket
import time
from datetime import datetime
from pathlib import Path

IP = "10.8.184.42"
PORT = 5025


def send(sock, cmd):
    sock.sendall((cmd + "\n").encode("ascii"))


def recv_until(sock, suffix=b"\n", timeout=3.0):
    sock.settimeout(timeout)
    data = b""

    while not data.endswith(suffix):
        chunk = sock.recv(1)
        if not chunk:
            break
        data += chunk

    return data


def query(sock, cmd):
    send(sock, cmd)
    return recv_until(sock).decode(errors="replace").strip()


def parse_measurement_value(reply):
    """
    Handles replies like:
      PAVA CUST1:MATH,MEAN,7.120000E-02W
      PAVA CUST1:C1,MEAN,1.400000E-02V
      MATH:PAVA MEAN,7.120000E-02W

    Returns the last numeric value in the reply.
    """
    nums = re.findall(r"[-+]?\d+(?:\.\d+)?(?:[Ee][-+]?\d+)?", reply)

    if not nums:
        raise ValueError(f"Could not parse numeric value from: {reply!r}")

    return float(nums[-1])


def parse_unit(reply):
    """
    Best-effort unit parse. Mostly for display/logging.
    """
    m = re.search(r"[-+]?\d+(?:\.\d+)?(?:[Ee][-+]?\d+)?\s*([a-zA-Z%]+)", reply)

    if not m:
        return ""

    return m.group(1)


def main():
    parser = argparse.ArgumentParser(
        description="Log power draw from Siglent SDS1202X-E measurement tool."
    )

    parser.add_argument("--duration", "-d", type=float, default=60.0)
    parser.add_argument("--interval", "-i", type=float, default=1.0)
    parser.add_argument("--csv-path", default="scope_power_log.csv")

    parser.add_argument(
        "--measurement",
        default="CUST1",
        help="Scope measurement to query. Usually CUST1, CUST2, etc. Default: CUST1",
    )

    parser.add_argument(
        "--scale",
        type=float,
        default=1.0,
        help="Multiply scope value by this. Use 0.001 if the scope returns mW but you want W.",
    )

    parser.add_argument(
        "--list-measurements",
        action="store_true",
        help="Print PAVA? CUSTALL and exit.",
    )

    args = parser.parse_args()

    csv_path = Path(args.csv_path)
    csv_path.parent.mkdir(parents=True, exist_ok=True)

    total_energy_J = 0.0

    with socket.create_connection((IP, PORT), timeout=5.0) as scope:
        print(query(scope, "*IDN?"))

        if args.list_measurements:
            print(query(scope, "PAVA? CUSTALL"))
            return

        print(f"measurement = {args.measurement}")
        print(f"scale       = {args.scale}")
        print(f"csv         = {csv_path}")
        print()
        print(f"Running {args.duration:.2f} second test...")
        print()

        with csv_path.open("w", newline="") as csv_file:
            writer = csv.writer(csv_file)

            writer.writerow([
                "timestamp",
                "elapsed_s",
                "power_W",
                "power_mW",
                "energy_J",
                "energy_Wh",
                "raw_scope_value",
                "raw_scope_unit",
                "raw_scope_reply",
            ])

            start = time.monotonic()
            last_t = start

            try:
                while True:
                    now = time.monotonic()
                    elapsed = now - start

                    if elapsed >= args.duration:
                        break

                    dt = now - last_t
                    last_t = now

                    ts = datetime.now().isoformat(timespec="milliseconds")

                    raw_reply = query(scope, f"PAVA? {args.measurement}")
                    raw_value = parse_measurement_value(raw_reply)
                    raw_unit = parse_unit(raw_reply)

                    power_W = raw_value * args.scale
                    power_mW = power_W * 1000.0

                    total_energy_J += power_W * dt
                    total_energy_Wh = total_energy_J / 3600.0

                    print(
                        f"{elapsed:8.2f}s | "
                        f"P={power_mW:10.3f} mW | "
                        f"E={total_energy_J:10.6f} J | "
                        f"Wh={total_energy_Wh:.9f} | "
                        f"raw={raw_reply}"
                    )

                    writer.writerow([
                        ts,
                        f"{elapsed:.6f}",
                        f"{power_W:.9f}",
                        f"{power_mW:.6f}",
                        f"{total_energy_J:.9f}",
                        f"{total_energy_Wh:.12f}",
                        f"{raw_value:.12g}",
                        raw_unit,
                        raw_reply,
                    ])

                    csv_file.flush()
                    time.sleep(args.interval)

            except KeyboardInterrupt:
                print()
                print("Stopped by user.")

    total_elapsed = time.monotonic() - start
    avg_power_W = total_energy_J / total_elapsed if total_elapsed > 0 else 0.0

    print()
    print("Test complete")
    print("-------------")
    print(f"Duration:      {total_elapsed:.3f} s")
    print(f"Average power: {avg_power_W * 1000.0:.3f} mW")
    print(f"Energy used:   {total_energy_J:.6f} J")
    print(f"Energy used:   {total_energy_J / 3600.0:.9f} Wh")


if __name__ == "__main__":
    main()
