import struct
import threading
import time
from typing import Optional

try:
    import minimalmodbus
except Exception:  # pragma: no cover - optional dependency at runtime
    minimalmodbus = None


DEFAULT_BAUD = 9600
DEFAULT_ADDRESS = 1


def make_instrument(port: str, baud: int, address: int):
    if minimalmodbus is None:
        raise RuntimeError("minimalmodbus is not installed")

    inst = minimalmodbus.Instrument(port, address, debug=False)
    inst.serial.baudrate = baud
    inst.serial.bytesize = 8
    inst.serial.parity = minimalmodbus.serial.PARITY_EVEN
    inst.serial.stopbits = 1
    inst.serial.timeout = 1.0
    inst.mode = minimalmodbus.MODE_RTU
    return inst


def read_once(inst) -> tuple[float, int]:
    regs = inst.read_registers(0, 4, functioncode=3)
    direction = int(regs[1])
    speed = struct.unpack(">f", struct.pack(">HH", regs[3], regs[2]))[0]
    return float(speed), direction


class AnemometerPoller:
    """Background poller for Jetson-attached ES-W302 anemometer."""

    def __init__(self, port: str, baud: int, address: int, interval_s: float) -> None:
        self.port = port
        self.baud = baud
        self.address = address
        self.interval_s = interval_s
        self._lock = threading.Lock()
        self._speed_mps: Optional[float] = None
        self._direction_deg: Optional[int] = None
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def latest(self) -> tuple[Optional[float], Optional[int]]:
        with self._lock:
            return self._speed_mps, self._direction_deg

    def _run(self) -> None:
        try:
            inst = make_instrument(self.port, self.baud, self.address)
        except Exception as exc:
            print(f"[ANEMO] disabled: {exc}")
            return

        errors = 0
        while not self._stop.is_set():
            try:
                speed, direction = read_once(inst)
                with self._lock:
                    self._speed_mps = speed
                    self._direction_deg = direction
                errors = 0
            except Exception as exc:
                errors += 1
                if errors <= 3:
                    print(f"[ANEMO] read warning: {exc}")
                try:
                    inst.serial.reset_input_buffer()
                except Exception:
                    pass
            self._stop.wait(self.interval_s)
