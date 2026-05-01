import argparse
import csv
import re
import socket
import time
from datetime import datetime

# ── CONFIG ─────────────────────────────────────
IP = "10.8.184.42"
PORT = 5025

DEFAULT_SHUNT_OHMS = 1.0

CURRENT_CH = "C1"
VOLTAGE_CH = "C2"
# ──────────────────────────────────────────────


def send(sock, cmd):
    sock.sendall((cmd + "\n").encode())


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


def siglent_float(reply):
    nums = re.findall(r"[-+]?\d+(?:\.\d+)?(?:[Ee][-+]?\d+)?", reply)
    if not nums:
        raise ValueError(f"Could not parse float from: {reply!r}")
    return float(nums[-1])


def parse_pava_value(reply, key):
    parts = reply.replace(" ", ",").split(",")

    for i, part in enumerate(parts):
        if part.upper() == key.upper() and i + 1 < len(parts):
            return siglent_float(parts[i + 1])

    raise ValueError(f"Could not find {key} in PAVA reply: {reply!r}")


def read_pava(sock, channel):
    reply = query(sock, f"{channel}:PAVA? ALL")
    if not reply:
        raise RuntimeError(f"No response from {channel}:PAVA? ALL")
    return reply


def main():
    parser = argparse.ArgumentParser(
        description="Log current draw from Siglent SDS1202X-E using a shunt resistor."
    )
    parser.add_argument("--duration", "-d", type=float, default=60)
    parser.add_argument("--interval", "-i", type=float, default=0.5)
    parser.add_argument("--shunt-ohms", type=float, default=DEFAULT_SHUNT_OHMS)
    parser.add_argument("--csv", action="store_true")
    parser.add_argument("--csv-path", default="scope_current_log.csv")
    parser.add_argument("--no-voltage", action="store_true")
    parser.add_argument("--supply-volts", type=float, default=None)
    parser.add_argument("--invert-current", action="store_true")
    parser.add_argument(
        "--current-measure",
        default="MEAN",
        help="PAVA measurement to use for CH1 shunt voltage. Default: MEAN",
    )
    parser.add_argument(
        "--voltage-measure",
        default="MEAN",
        help="PAVA measurement to use for CH2 supply voltage. Default: MEAN",
    )

    args = parser.parse_args()

    use_voltage = (
        args.supply_volts is not None
        or (VOLTAGE_CH is not None and not args.no_voltage)
    )

    total_charge_As = 0.0
    total_energy_J = 0.0

    with socket.create_connection((IP, PORT), timeout=5.0) as scope:
        print(query(scope, "*IDN?"))
        print(f"shunt = {args.shunt_ohms} ohm")
        print(f"current source = {CURRENT_CH}:{args.current_measure}")

        if args.supply_volts is not None:
            print(f"voltage source = fixed {args.supply_volts} V")
        elif not args.no_voltage:
            print(f"voltage source = {VOLTAGE_CH}:{args.voltage_measure}")

        csv_file = None
        writer = None

        if args.csv:
            csv_file = open(args.csv_path, "w", newline="")
            writer = csv.writer(csv_file)
            writer.writerow([
                "timestamp",
                "elapsed_s",
                "vshunt_V",
                "vshunt_min_V",
                "vshunt_max_V",
                "current_mA",
                "current_min_mA",
                "current_max_mA",
                "vsupply_V",
                "vsupply_min_V",
                "vsupply_max_V",
                "power_mW",
                "accumulated_mAh",
                "accumulated_Ah_per_min",
            ])

        print()
        print(f"Running {args.duration:.2f} second test...")
        print()

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

                current_pava = read_pava(scope, CURRENT_CH)

                vshunt = parse_pava_value(current_pava, args.current_measure)
                vshunt_min = parse_pava_value(current_pava, "MIN")
                vshunt_max = parse_pava_value(current_pava, "MAX")

                current_A = vshunt / args.shunt_ohms
                current_min_A = vshunt_min / args.shunt_ohms
                current_max_A = vshunt_max / args.shunt_ohms

                if args.invert_current:
                    vshunt = -vshunt
                    vshunt_min, vshunt_max = -vshunt_max, -vshunt_min

                    current_A = -current_A
                    current_min_A, current_max_A = -current_max_A, -current_min_A

                total_charge_As += current_A * dt

                if args.supply_volts is not None:
                    v_avg = args.supply_volts
                    v_min = args.supply_volts
                    v_max = args.supply_volts
                elif not args.no_voltage:
                    voltage_pava = read_pava(scope, VOLTAGE_CH)
                    v_avg = parse_pava_value(voltage_pava, args.voltage_measure)
                    v_min = parse_pava_value(voltage_pava, "MIN")
                    v_max = parse_pava_value(voltage_pava, "MAX")
                else:
                    v_avg = None
                    v_min = None
                    v_max = None

                if v_avg is not None:
                    power_W = v_avg * current_A
                    power_mW = power_W * 1000.0
                    total_energy_J += power_W * dt
                else:
                    power_mW = None

                mAh = total_charge_As / 3600.0 * 1000.0
                Ah_per_min = (
                    (total_charge_As / 3600.0) / (elapsed / 60.0)
                    if elapsed > 0 else 0.0
                )

                print(
                    f"{elapsed:8.2f}s | "
                    f"Vshunt={vshunt * 1000:9.3f} mV | "
                    f"I={current_A * 1000:9.3f} mA | "
                    f"Imin={current_min_A * 1000:9.3f} | "
                    f"Imax={current_max_A * 1000:9.3f} | "
                    f"mAh={mAh:10.6f} | "
                    f"Ah/min={Ah_per_min:10.8f}",
                    end="",
                )

                if v_avg is not None:
                    print(
                        f" | Vavg={v_avg:6.3f} V | "
                        f"Vmin={v_min:6.3f} | "
                        f"Vmax={v_max:6.3f} | "
                        f"P={power_mW:9.3f} mW",
                        end="",
                    )

                print()

                if writer:
                    writer.writerow([
                        ts,
                        elapsed,
                        vshunt,
                        vshunt_min,
                        vshunt_max,
                        current_A * 1000.0,
                        current_min_A * 1000.0,
                        current_max_A * 1000.0,
                        v_avg,
                        v_min,
                        v_max,
                        power_mW,
                        mAh,
                        Ah_per_min,
                    ])
                    csv_file.flush()

                time.sleep(args.interval)

        finally:
            if csv_file:
                csv_file.close()

    total_elapsed = time.monotonic() - start
    total_Ah = total_charge_As / 3600.0
    total_mAh = total_Ah * 1000.0
    avg_current_A = total_charge_As / total_elapsed if total_elapsed > 0 else 0.0
    avg_Ah_per_min = total_Ah / (total_elapsed / 60.0) if total_elapsed > 0 else 0.0

    print()
    print("Test complete")
    print("-------------")
    print(f"Duration:        {total_elapsed:.3f} s")
    print(f"Average current: {avg_current_A * 1000.0:.3f} mA")
    print(f"Capacity used:   {total_mAh:.6f} mAh")
    print(f"Ah/min:          {avg_Ah_per_min:.8f} Ah/min")

    if use_voltage:
        print(f"Energy used:     {total_energy_J:.6f} J")
        print(f"Energy used:     {total_energy_J / 3600.0:.9f} Wh")


if __name__ == "__main__":
    main()
