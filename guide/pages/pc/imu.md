# Add and calibrate the IMU

You need the Adafruit BNO055 breakout, a drive layout, and the app connected. Keep the board disarmed and the motor supply off. You’ll see the orientation update in the app and save a calibration profile for this sensor.

1. Unplug USB and wire the breakout using the [IMU pin table]({{base}}pc/wire/#imu-pins). Check the power and ground before reconnecting USB and opening the app.
2. In **Layout**, choose **Add → Instance**. Select `imu.bno055`, label it `imu`, and choose the sensor loop. It should appear as a pending instance on that loop.
3. In **Params**, set `sda_pin` to 8, `scl_pin` to 9, and check that `address` is 40 (hexadecimal `0x28`). Use **Review**, then **Save and reboot**. When the app reconnects, the instance should be running on the sensor loop.
4. Select **imu**, then **Calibration**. You should see four calibration levels and an orientation preview. Gently tilt the sensor and check that the displayed pose changes. If the panel has no live data, check the wiring and address before continuing.
5. Follow the guidance in the panel. Hold the board still for the gyroscope, rest it in stable orientations for the accelerometer, and rotate it for the magnetometer. You can follow the [BNO055 calibration instructions](https://learn.adafruit.com/adafruit-bno055-absolute-orientation-sensor/device-calibration) for the sensor motions. Continue until **System**, **Gyroscope**, **Accelerometer**, and **Magnetometer** each read 3.
6. Select **Read from IMU** to stage the profile, then **Save** to keep it across restart. **Unsaved** should clear. Use **Export** if you want a copy on your PC. Keep it with a note about this sensor’s mounting.
7. Reboot while disarmed and reopen **Calibration**. Check that live samples return and the orientation responds. If you restore a file later, use **Import**, review the target sensor, then **Apply**. Wait for **Applied** before selecting **Save**.

{{figure:layout-calibration-applied}}

Continue to [Update firmware]({{base}}pc/update/) when a newer DriveLink release is available.
