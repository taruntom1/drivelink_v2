# Glossary

Look up a term when you see it on the bench or in the app.

| Term | Meaning |
| --- | --- |
| Agent | A Linux service that connects clients to the ESP. See [Agent, leases, and tokens]({{base}}concepts/agent/). |
| App image | The firmware file you install from the app’s **Firmware update** card. |
| apt | Ubuntu’s tool for installing and updating packages and their dependencies. See [Install from apt]({{base}}apt/). |
| Arm | Allow the board to act on motion commands. See [Arming and faults]({{base}}concepts/safety/). |
| Baud rate | The speed of a serial link. See [Firmware and serial]({{base}}concepts/firmware/). |
| Calibration | Corrections a sensor learns to improve its measurements. See [IMU]({{base}}concepts/imu/). |
| Controller | A module that takes a command and sets motor output. See [Actuators, encoders, and controllers]({{base}}concepts/control/). |
| Duty | The on fraction of the motor drive signal. A low cap limits the maximum drive requested. |
| Encoder | A sensor for the rotation of a shaft. A quadrature encoder uses A and B signals to tell direction. |
| ESP32-S3 | The microcontroller on the DriveLink board. |
| Fault | A reported problem that may block arming or disarm the device. |
| Firmware | The software stored on the ESP. |
| Flash | Memory that holds firmware and settings. To flash means to write an image to that memory. |
| Gear ratio | Motor shaft turns per output shaft turn. |
| GPIO | A numbered board pin you configure as an input or output. |
| I²C | A clock-and-data connection used by the BNO055. |
| IMU | Inertial measurement unit, a sensor that measures motion and can estimate orientation. |
| Instance | One configured use of a module type. See [Modules, instances, and loops]({{base}}concepts/modules/). |
| Layout | The set of loops, instances, and connections configured on the board. |
| Lease | Permission for one client to send motion commands through the agent. |
| Loop | A schedule on which a group of instances runs. |
| Merged image | The complete firmware image used for first flash. |
| Observer | A remote client that can read device state without a motion lease. |
| Parameter | A setting you can change on the board, such as a pin number. |
| PID | Proportional, integral, and derivative control. See [Actuators, encoders, and controllers]({{base}}concepts/control/). |
| PWM and DIR | Pulse-width modulation sets duty. DIR selects direction. |
| ROS 2 | Software tools and libraries that let robot programs communicate. See [ROS 2 in five minutes]({{base}}concepts/ros/). |
| Safe mode | A boot state that keeps motion disabled while you investigate a problem. |
| Signal | A measured or computed value you can watch in the app. |
| Token | A shared secret a client presents to connect to an agent. |
| UART | The board’s serial connection. A USB-to-UART bridge lets your computer use it over USB. |
| Watchdog | A timer that forces commands to safe values when updates stop arriving. |
