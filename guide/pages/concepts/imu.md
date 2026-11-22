# IMU

If you’re deciding whether to add a sensor, this page explains what an IMU adds to the bench and what calibration does.

An **inertial measurement unit** (IMU) measures motion. The reference BNO055 combines an accelerometer, a gyroscope, and a magnetometer. It uses their readings to estimate its orientation, or which way it faces. A wheel encoder still measures the wheel’s own rotation.

The ESP reads the BNO055 over **I²C**, a two-wire connection with a data wire and a clock wire. How you mount the sensor determines which direction each of its axes points.

**Calibration** lets the sensor learn corrections. The app shows system, gyroscope, accelerometer, and magnetometer levels from 0 to 3. The saved profile belongs to that sensor and its mounting. Check the readings again if you move it or change nearby metal or magnetic parts.

See the [Adafruit sensor guide](https://learn.adafruit.com/adafruit-bno055-absolute-orientation-sensor) for the breakout board, or [add and calibrate the IMU]({{base}}pc/imu/) in DriveLink.
