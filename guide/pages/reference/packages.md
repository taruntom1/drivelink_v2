# Packages, versions, and limits

Use this page to choose packages for your computer and check whether a device or network setting is within its supported range.

| Component | Where to get it |
| --- | --- |
| Firmware | The ESP32-S3 release ZIP. Use the merged image to [flash a new board]({{base}}flash/) and the app image to [update]({{base}}pc/update/). |
| Desktop app | The Windows x64 installer or the `drivelink` package for Ubuntu 26.04 on amd64 or arm64. |
| Agent and companion CLI | `drivelink-agent` and `drivelink-cli` for Ubuntu 24.04 or 26.04 on amd64 or arm64. |
| ROS 2 | `drivelink-protocol`, `drivelink`, `drivelink-hardware`, and `drivelink-bringup`, prefixed in apt with `ros-lyrical-` or `ros-jazzy-`. The bringup package installs the others. |

Get files for the same release from [Downloads]({{base}}downloads/) or the [signed apt repository]({{base}}apt/). A firmware and app mismatch can prevent connecting. The remote connection also checks the agent protocol version.

| Limit | Reference |
| --- | --- |
| 4 device loops by default, including the two factory loops. You can raise the limit to 8. | Change it in [Device settings]({{base}}pc/settings/) and check the count in [Layout]({{base}}pc/layout/). |
| Up to 6 PWM + DIR instances and 4 quadrature feedback instances | Check the [reference build]({{base}}reference/bench/) for usable pins and electrical limits. |
| Agent up to 8 devices and one controller client per device | See [agent settings]({{base}}reference/agent/) and [sharing control]({{base}}pi/control/). |
| Firmware UART at 921,600 baud | See [Pi UART wiring]({{base}}pi/wire/). |
| Watchdog 20–1,000 ms, 200 ms by default | See [Arming and faults]({{base}}concepts/safety/). |
