import adafruit_bme680


class BME688:
    DEFAULT_ADDRESS = 0x77

    def __init__(self, i2c, address=DEFAULT_ADDRESS):
        self._bme = adafruit_bme680.Adafruit_BME680_I2C(
            i2c,
            address=address,
        )

    def sample(self):
        return {
            "sensor": "bme688",
            "temperature_c": self._bme.temperature,
            "humidity_percent": self._bme.relative_humidity,
            "pressure_hpa": self._bme.pressure,
            "gas_resistance_ohm": self._bme.gas,
        }
