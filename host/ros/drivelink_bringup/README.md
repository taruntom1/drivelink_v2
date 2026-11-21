# drivelink_bringup

An example two-wheel diff-drive robot with an IMU for ROS 2 Lyrical and Jazzy.
DriveLink connects to the device through `drivelink-agent`. The URDF maps
the two velocity-controlled joints and the IMU to device instance labels.
The same wheel separation in the controller YAML sets the URDF joint origins.

## Install

### From apt

On Ubuntu 26.04 use Lyrical. On Ubuntu 24.04 use Jazzy. Enable the matching
[ROS 2 apt source](https://github.com/ros-infrastructure/ros-apt-source) and
[DriveLink's signed apt source](../../../docs/pi-setup.md#4-install-the-packages).
With both sources enabled, install from a DriveLink release that includes ROS
packages.

```sh
distro=lyrical  # Use jazzy on Ubuntu 24.04.
sudo apt update
sudo apt install "ros-$distro-ros-base" "ros-$distro-drivelink-bringup"
source "/opt/ros/$distro/setup.bash"
```

Install `drivelink-agent` on the computer that owns the device port.

### From source

From a colcon workspace with this repository at `src/drivelink_v2` and ROS
installed, run:

```sh
sudo sh src/drivelink_v2/tools/setup_linux.sh ros
source /opt/ros/$ROS_DISTRO/setup.bash
colcon build --packages-up-to drivelink_bringup
source install/setup.bash
```

## Launch

The packaged agent keeps `/etc/drivelink/token` at `0640 root:drivelink`.
Add the ROS user with `sudo usermod -aG drivelink "$USER"` and log in again.
Start the agent and substitute your device alias or 12-hex ID for `robot`.

```sh
ros2 launch drivelink_bringup example.launch.py device:=robot
```

The launch accepts `agent` (default `tcp://127.0.0.1:47620`), `device`
(default `dev`), `token_file` (default `/etc/drivelink/token`), and
`left_label`, `right_label`, and `imu_label` (defaults `left`, `right`,
and `imu`). An inline `token` takes precedence for testing. Prefer a token
file on robots because ROS publishes the robot description.

For a real robot, copy the example `config/controllers.yaml` and supply it
via `controllers_file:=/path/to/robot.yaml`. Put the measured wheel radius
and wheel separation in the `diff_drive_controller.ros__parameters`
section. Use your actual device labels and verify joint and IMU axes during
the first run on stands.

The hardware component and state broadcasters start active. The drive
controller starts inactive and the device stays disarmed. When the robot is
ready to move, explicitly activate the drive controller:

```sh
ros2 control switch_controllers --activate diff_drive_controller --strict --switch-timeout 10
```

The drive accepts fresh `geometry_msgs/msg/TwistStamped` on `/cmd_vel`.
The example publishes `/joint_states`, `/odom`, `/imu`, and the corresponding
TF frames. It commands zero velocity when `cmd_vel` goes quiet for 0.25 s.
Deactivate the drive controller to disarm.

## Diagnostics and recovery

`/diagnostics` includes the device's online state, lease holder, arm state,
active error count, last disarm reason, and most recent diagnostic record.
The diagnostics node uses its own agent connection and reconnects as an
observer. See [diagnostic codes](../../../docs/protocol/diag-codes.md).

`ack_fault` only acknowledges the device fault. It can succeed once the
fault condition clears. `recover` also activates the hardware and the
controllers specified in `recovery_controllers` (comma-separated launch
argument, defaulting to the two broadcasters and the drive controller).
Activating the drive controller arms the device.

After a fault, make sure the robot can move safely, clear the fault condition
and release any GUI or other client lease before requesting recovery.

```sh
ros2 service call /drivelink_diagnostics/ack_fault std_srvs/srv/Trigger '{}'
ros2 service call /drivelink_diagnostics/recover std_srvs/srv/Trigger '{}'
```

Call `ack_fault` on its own when the robot should stay disarmed. To recover
manually, acknowledge the fault, then use
`ros2 control set_hardware_component_state drivelink inactive` if the
component is unconfigured, then `ros2 control set_hardware_component_state
drivelink active`. Activate the configured controllers that are inactive.
After a timeout or unconfirmed cleanup, check the hardware, controller and
arm states before requesting another recovery. Deactivate any controller still
claiming a hardware command interface before retrying.

## Checks

`python tools/validate.py --only ros` runs the recovery tests and the installed
example launch against vdev behind the agent. `ros-jazzy` runs the same checks
in the Jazzy container. The mock and real-robot driving checks share
[`host/test/ros_bringup_check.py`](../../test/ros_bringup_check.py). The real
run uses an interactive terminal and the first-run safety procedure.
`ros-deb-install` runs the mock check from apt-installed packages in clean
containers.
