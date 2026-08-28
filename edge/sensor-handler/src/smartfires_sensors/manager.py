import os

os.environ.setdefault(
    "JETSON_MODEL_NAME",
    "JETSON_ORIN_NANO",
)

from adafruit_extended_bus import ExtendedI2C

from .sensors.gps import PA1010D
from .sensors.imu import ICM20948
from .sensors.bme688 import BME688
from .sensors.anemometer import ESW302


class SensorManager:

    def __init__(
        self,
        i2c_bus=7,
        anemometer_port="/dev/ttyUSB0",
    ):
        self.i2c = ExtendedI2C(i2c_bus)

        self.gps = PA1010D(self.i2c)

        self.imu = ICM20948(
            self.i2c,
            address=0x69,
        )

        self.bme688 = BME688(
            self.i2c,
            address=0x77,
        )

        self.anemometer = ESW302(
            port=anemometer_port,
        )
