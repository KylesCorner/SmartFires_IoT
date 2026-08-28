import struct

import minimalmodbus


DEFAULT_BAUD = 9600
DEFAULT_ADDRESS = 1


class ESW302:
    def __init__(
        self,
        port: str,
        baud: int = DEFAULT_BAUD,
        address: int = DEFAULT_ADDRESS,
    ):
        self._instrument = minimalmodbus.Instrument(
            port,
            address,
            debug=False,
        )

        self._instrument.serial.baudrate = baud
        self._instrument.serial.bytesize = 8
        self._instrument.serial.parity = (
            minimalmodbus.serial.PARITY_EVEN
        )
        self._instrument.serial.stopbits = 1
        self._instrument.serial.timeout = 1.0

        self._instrument.mode = minimalmodbus.MODE_RTU

    def sample(self):
        regs = self._instrument.read_registers(
            0,
            4,
            functioncode=3,
        )

        direction = int(regs[1])

        speed = struct.unpack(
            ">f",
            struct.pack(
                ">HH",
                regs[3],
                regs[2],
            ),
        )[0]

        return {
            "sensor": "anemometer",
            "speed_mps": float(speed),
            "direction_deg": direction,
        }
