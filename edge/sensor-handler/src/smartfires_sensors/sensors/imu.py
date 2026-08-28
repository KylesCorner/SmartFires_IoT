import math

import adafruit_icm20x


class ICM20948:
    DEFAULT_ADDRESS = 0x69

    def __init__(self, i2c, address=DEFAULT_ADDRESS):
        self._imu = adafruit_icm20x.ICM20948(
            i2c,
            address=address,
        )

    def sample(self):
        ax, ay, az = self._imu.acceleration
        gx, gy, gz = self._imu.gyro
        mx, my, mz = self._imu.magnetic

        return {
            "sensor": "imu",

            "accel_x_mps2": ax,
            "accel_y_mps2": ay,
            "accel_z_mps2": az,

            "gyro_x_dps": math.degrees(gx),
            "gyro_y_dps": math.degrees(gy),
            "gyro_z_dps": math.degrees(gz),

            "mag_x_ut": mx,
            "mag_y_ut": my,
            "mag_z_ut": mz,
        }
