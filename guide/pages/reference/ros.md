# ROS 2 hardware and launch reference

Use this reference with [your own robot description]({{base}}ros/robot/) or the [installed example]({{base}}ros/example/). The DriveLink hardware system connects through the agent.

## Hardware parameters

| Parameter | Default | What it controls |
| --- | --- | --- |
| `device` | Required | Agent alias or 12-digit hexadecimal device ID. |
| `agent` | `tcp://127.0.0.1:47620` | Agent address. |
| `token_file` | `/etc/drivelink/token` | File containing the token on its first line. |
| `token` | Empty | Inline token for tests. Takes precedence over the token file. A robot description can expose inline parameters. |
| `take_lease` | `refuse` | Refuse activation when another client holds the lease. `preempt` takes the lease and disarms the device before transfer. |
| `state_rate_hz` | `100` | Requested state rate in Hz, from 1 to 10,000. The effective rate follows the device loop and its available dividers. |
| `stale_timeout_ms` | `auto` | How old a sample may be before causing an error. Automatic uses ten sample periods, with a 100 ms minimum. `0` disables the age check. |
| `timeout_ms` | `3000` | Timeout for agent and device requests during setup and command-mode switching. |

## Hardware interfaces

Each joint or sensor uses an `instance` parameter to select its DriveLink label. State interfaces ordinarily read the same-named device signals, with the IMU mappings below. Use `state.INTERFACE` to choose a different signal.

| State interface | Device signal and unit |
| --- | --- |
| Joint `position` | `position` in radians |
| Joint `velocity` | `velocity` in radians per second |
| Joint `duty` | `duty`, signed fraction from −1 to 1 |
| IMU `orientation.w/x/y/z` | `quaternion.w/x/y/z` |
| IMU `angular_velocity.x/y/z` | `gyro.x/y/z` in radians per second |
| IMU `linear_acceleration.x/y/z` | `accel.x/y/z` in metres per second squared, including gravity |

| Joint command | Device input | Mode |
| --- | --- | --- |
| `position` | `position_sp` in radians | `position` |
| `velocity` | `velocity_sp` in radians per second | `velocity` |
| `duty` | `duty_sp`, signed fraction from −1 to 1 | `open_loop` |

Use `command.INTERFACE` and `mode.INTERFACE` to override the input and mode. Use `mode_param` to change the parameter name from `mode`, or set it to `none` to omit the mode write. Each joint can have one active command interface at a time. Claiming the first command interface arms the device. Releasing the last disarms it. See [ROS recovery]({{base}}ros/recover/) after an error.

## Bringup launch arguments

| Argument | Default | What to change |
| --- | --- | --- |
| `agent` | `tcp://127.0.0.1:47620` | Set this if the agent is on another computer. |
| `device` | `dev` | Use your agent device alias or ID. |
| `token_file` | `/etc/drivelink/token` | Point to the token on the ROS computer. |
| `token` | Empty | Inline override for tests. |
| `left_label`, `right_label`, `imu_label` | `left`, `right`, `imu` | Use the label of each running device instance. |
| `controllers_file` | Installed `config/controllers.yaml` | Use a copy with your measured wheel radius and separation. |
| `recovery_controllers` | `joint_state_broadcaster,imu_sensor_broadcaster,diff_drive_controller` | Comma-separated list of controllers to restore on recovery. Including the drive controller makes recovery arm the device. |
